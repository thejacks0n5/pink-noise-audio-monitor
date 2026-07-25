#pragma once
/*
  ==============================================================================
    AnalysisProcessor.h
    Pink Noise Audio Monitor (pnam) — per-block measurement on the audio thread.

    Responsibilities (all on the AUDIO thread, inside the device callback,
    called after PinkNoiseGenerator::processBlock has produced the reference):
      1. Compute per-block peak and RMS for the reference (via ReferenceHistory
         / last generated block) and for each returned input channel.
      2. Accumulate those into a measurement interval (default 1.0 s) and, at
         each interval boundary, push one pnam::Measurement into the
         MeasurementQueue (drop-on-full, never blocks).
      3. Maintain peak-hold values and publish live meter levels through
         std::atomic<float> getters that the UI polls from its timer.
      4. Return a BlockStats summary for the current block so the AudioEngine
         can feed the EventDetector without recomputing anything.

    correlation is 0.0f in V1 (field reserved; ReferenceHistory retains the
    data needed to implement delay-compensated correlation in V2).

    Never allocates, locks, writes files, or touches the UI.
  ==============================================================================
*/

#include <atomic>
#include <cstdint>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../Model/DataModel.h"
#include "MeasurementQueue.h"
#include "ReferenceHistory.h"

namespace pnam
{

//==============================================================================
/** Cheap per-block summary handed from AnalysisProcessor to EventDetector
    within the same audio callback. All dB values are dBFS; silence is
    silenceFloorDb. POD. */
struct BlockStats
{
    double  blockStartElapsedSeconds { 0.0 }; ///< Session-relative start time of this block.
    int     numSamples               { 0 };
    int     numInputChannels         { 0 };

    float   refPeakDb  { silenceFloorDb };    ///< Reference block peak (post-gain).
    float   refRmsDb   { silenceFloorDb };
    float   inPeakDb[2]  { silenceFloorDb, silenceFloorDb };  ///< [0]=L, [1]=R (R mirrors L in mono).
    float   inRmsDb[2]   { silenceFloorDb, silenceFloorDb };

    float   gainDiffDb { 0.0f };              ///< max(inRmsDb) - refRmsDb.
    int32_t clipCount  { 0 };                 ///< Input samples at/above clip threshold, all channels.
    int32_t statusCode { StatusBits::ok };    ///< StatusBits snapshot (mute/fade/settling...).
};

static_assert (std::is_trivially_copyable<BlockStats>::value,
               "BlockStats is passed by value on the audio thread");

//==============================================================================
class AnalysisProcessor
{
public:
    /** @param queueToFill  the audio->message measurement FIFO. Must outlive
        this object. */
    explicit AnalysisProcessor (MeasurementQueue& queueToFill);

    AnalysisProcessor (const AnalysisProcessor&) = delete;
    AnalysisProcessor& operator= (const AnalysisProcessor&) = delete;

    //==========================================================================
    /** NON-REAL-TIME, audio stopped. Resets accumulators, peak holds, and the
        session-relative sample clock to zero. */
    void prepare (double sampleRate, int maximumBlockSize, int numInputChannels);

    /** Interval between Measurement pushes, seconds. Clamped to [0.1, 60].
        Default 1.0. Any thread (atomic; takes effect at the next boundary). */
    void setMeasurementIntervalSeconds (double seconds) noexcept;
    double getMeasurementIntervalSeconds() const noexcept;

    /** Linear amplitude at/above which an input sample counts as clipped.
        Shared definition with EventDetector — the AudioEngine sets both from
        one config value. Default 0.999. Any thread. */
    void setClipThresholdLinear (float threshold) noexcept;

    //==========================================================================
    /** AUDIO THREAD. Analyse one block of returned input against the newest
        reference samples.

        @param input      device input channel pointers.
        @param numInCh    number of valid input channels (1 or 2).
        @param numSamples block length.
        @param reference  history written earlier in this same callback by the
                          generator; the newest numSamples are used as the
                          reference block (V1: zero delay compensation).
        @param statusBitsFromEngine  StatusBits flags the engine knows about
                          (outputMuted / fadingIn / fadingOut / settling /
                          inputInactive) — merged into stats and Measurement.

        @returns the per-block stats for the EventDetector. Never allocates,
                 locks, or blocks. */
    BlockStats processBlock (const float* const* input, int numInCh, int numSamples,
                             const ReferenceHistory& reference,
                             int32_t statusBitsFromEngine) noexcept;

    /** AUDIO THREAD. Called by EventDetector (via AudioEngine) so the interval
        Measurement can carry dropoutFlag; sets StatusBits::dropoutActive. */
    void noteDropoutActive() noexcept;

    /** MESSAGE THREAD, and only while the device callback is detached (i.e. from
        AudioEngine::start()). Zeroes the session clock.

        prepare() deliberately does NOT do this: it runs on every device restart,
        and zeroing there made elapsedSeconds — and therefore the CSV's derived
        timestamp_utc — jump backwards mid-session every time the device was
        reconfigured. The clock now carries forward across restarts and only
        resets on a genuine session start. */
    void resetSessionClock() noexcept;

    //==========================================================================
    // Live meter values — atomics polled by the UI timer. dBFS.

    float getRefPeakDb()  const noexcept { return atomicRefPeakDb.load (std::memory_order_relaxed); }
    float getRefRmsDb()   const noexcept { return atomicRefRmsDb.load (std::memory_order_relaxed); }

    /** channel: 0 = L, 1 = R. Mono sessions mirror channel 0 into 1. */
    float getInPeakDb (int channel) const noexcept
    {
        return atomicInPeakDb[juce::jlimit (0, 1, channel)].load (std::memory_order_relaxed);
    }
    float getInRmsDb (int channel) const noexcept
    {
        return atomicInRmsDb[juce::jlimit (0, 1, channel)].load (std::memory_order_relaxed);
    }

    /** Held maxima since the last resetPeakHold(). */
    float getRefPeakHoldDb() const noexcept { return atomicRefPeakHoldDb.load (std::memory_order_relaxed); }
    float getInPeakHoldDb (int channel) const noexcept
    {
        return atomicInPeakHoldDb[juce::jlimit (0, 1, channel)].load (std::memory_order_relaxed);
    }

    /** Any thread (UI button). Clears held peaks to silenceFloorDb. */
    void resetPeakHold() noexcept;

    /** Session-relative audio-clock time in seconds (samples processed /
        sample rate). Atomic; the message thread uses this as `sessionClock`
        when stamping rows. */
    double getElapsedSeconds() const noexcept;

private:
    //==========================================================================
    MeasurementQueue& measurementQueue;

    // Config (atomic targets)
    std::atomic<float>  measurementIntervalSeconds { 1.0f };
    std::atomic<float>  clipThresholdLinear        { 0.999f };

    // Audio-thread accumulation state
    double  sampleRate { 0.0 };
    int     maxBlock   { 0 };
    int     numInCh    { 0 };
    int64_t samplesProcessed        { 0 };

    // Elapsed time accumulated by previous device configurations in this
    // session. samplesProcessed counts only the current one, so that a
    // sample-rate change converts correctly instead of warping the clock.
    double  elapsedOffsetSeconds    { 0.0 };
    int64_t samplesIntoInterval     { 0 };
    // interval accumulators: sum of squares, running peaks, clip count, flags
    double  refSumSquares { 0.0 };
    double  inSumSquares[2] { 0.0, 0.0 };
    float   refIntervalPeak { 0.0f };
    float   inIntervalPeak[2] { 0.0f, 0.0f };
    int32_t intervalClipCount { 0 };
    int32_t intervalStatusBits { StatusBits::ok };
    bool    intervalDropoutSeen { false };

    // Published live values
    std::atomic<float>  atomicRefPeakDb { silenceFloorDb };
    std::atomic<float>  atomicRefRmsDb  { silenceFloorDb };
    std::atomic<float>  atomicInPeakDb[2];
    std::atomic<float>  atomicInRmsDb[2];
    std::atomic<float>  atomicRefPeakHoldDb { silenceFloorDb };
    std::atomic<float>  atomicInPeakHoldDb[2];
    std::atomic<double> atomicElapsedSeconds { 0.0 };
};

} // namespace pnam
