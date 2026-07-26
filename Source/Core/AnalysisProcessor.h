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

    int     numOutputChannels        { 0 };

    float   refPeakDb[maxChannels] {};        ///< Per-output reference block peak (post-gain).
    float   refRmsDb[maxChannels]  {};
    float   inPeakDb[maxChannels]  {};        ///< Per-input block peak.
    float   inRmsDb[maxChannels]   {};

    /** Per-input: inRmsDb[i] - refRmsDb[reference output for input i]. */
    float   gainDiffDb[maxChannels] {};

    /** Which output each input was compared against, so the EventDetector can
        quote the right reference level without needing the mapping itself. */
    int32_t referenceOutputForInput[maxChannels] {};

    /** Whether each input participates in fault detection (see
        SessionInfo::inputMonitored). Levels are measured regardless, but
        unmonitored inputs are omitted from the CSV. */
    bool    inputMonitored[maxChannels] {};
    int32_t clipCount  { 0 };                 ///< Input samples at/above clip threshold, all channels.
    int32_t clipCountPerChannel[maxChannels] {};  ///< Same, split per input channel.
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
    void prepare (double sampleRate, int maximumBlockSize, int numInputChannels,
                  int numOutputChannels);

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

    /** MESSAGE THREAD, and only while the device callback is detached.
        AudioEngine::start() removes the callback around this call precisely
        because the callback now stays attached while stopped (to keep input
        meters live), so resetting underneath it would be a data race.
        Zeroes the session clock.

        prepare() deliberately does NOT do this: it runs on every device restart,
        and zeroing there made elapsedSeconds — and therefore the CSV's derived
        timestamp_utc — jump backwards mid-session every time the device was
        reconfigured. The clock now carries forward across restarts and only
        resets on a genuine session start. */
    void resetSessionClock() noexcept;

    //==========================================================================
    // Live meter values — atomics polled by the UI timer. dBFS.

    /** All take a channel index clamped to [0, maxChannels). Output-side getters
        index output channels; input-side getters index input channels. */
    float getRefPeakDb (int channel = 0) const noexcept
    {
        return atomicRefPeakDb[clampChannel (channel)].load (std::memory_order_relaxed);
    }
    float getRefRmsDb (int channel = 0) const noexcept
    {
        return atomicRefRmsDb[clampChannel (channel)].load (std::memory_order_relaxed);
    }

    float getInPeakDb (int channel) const noexcept
    {
        return atomicInPeakDb[clampChannel (channel)].load (std::memory_order_relaxed);
    }
    float getInRmsDb (int channel) const noexcept
    {
        return atomicInRmsDb[clampChannel (channel)].load (std::memory_order_relaxed);
    }

    /** Held maxima since the last resetPeakHold(). */
    float getRefPeakHoldDb (int channel = 0) const noexcept
    {
        return atomicRefPeakHoldDb[clampChannel (channel)].load (std::memory_order_relaxed);
    }
    float getInPeakHoldDb (int channel) const noexcept
    {
        return atomicInPeakHoldDb[clampChannel (channel)].load (std::memory_order_relaxed);
    }

    /** Live channel counts, as last prepared. */
    int getNumInputChannels()  const noexcept { return numInCh; }
    int getNumOutputChannels() const noexcept { return numOutCh; }

    /** MESSAGE THREAD (audio stopped, or benign: a plain atomic store).
        Chooses which output channel an input is compared against. Defaults to
        the same index, clamped to the available outputs. */
    void setReferenceOutputForInput (int inputChannel, int outputChannel) noexcept
    {
        referenceOutputForInput[clampChannel (inputChannel)]
            .store (clampChannel (outputChannel), std::memory_order_relaxed);
    }

    int getReferenceOutputForInput (int inputChannel) const noexcept
    {
        return referenceOutputForInput[clampChannel (inputChannel)].load (std::memory_order_relaxed);
    }

    /** MESSAGE THREAD. Excludes an input from fault detection without hiding its
        levels — for inputs with nothing patched into them, which would otherwise
        sit below the dropout threshold and report a permanent critical fault. */
    void setInputMonitored (int inputChannel, bool shouldMonitor) noexcept
    {
        inputMonitoredFlags[clampChannel (inputChannel)].store (shouldMonitor, std::memory_order_relaxed);
    }

    bool isInputMonitored (int inputChannel) const noexcept
    {
        return inputMonitoredFlags[clampChannel (inputChannel)].load (std::memory_order_relaxed);
    }

    /** Any thread (UI button). Clears held peaks to silenceFloorDb. */
    void resetPeakHold() noexcept;

    /** Session-relative audio-clock time in seconds (samples processed /
        sample rate). Atomic; the message thread uses this as `sessionClock`
        when stamping rows. */
    double getElapsedSeconds() const noexcept;

private:
    //==========================================================================
    static int clampChannel (int ch) noexcept { return juce::jlimit (0, maxChannels - 1, ch); }

    MeasurementQueue& measurementQueue;

    // Config (atomic targets)
    std::atomic<float>  measurementIntervalSeconds { 1.0f };
    std::atomic<float>  clipThresholdLinear        { 0.999f };

    // Audio-thread accumulation state
    double  sampleRate { 0.0 };
    int     maxBlock   { 0 };
    int     numInCh    { 0 };
    int     numOutCh   { 0 };
    int64_t samplesProcessed        { 0 };

    // Elapsed time accumulated by previous device configurations in this
    // session. samplesProcessed counts only the current one, so that a
    // sample-rate change converts correctly instead of warping the clock.
    double  elapsedOffsetSeconds    { 0.0 };
    int64_t samplesIntoInterval     { 0 };
    // interval accumulators: sum of squares, running peaks, clip count, flags
    double  refSumSquares[maxChannels] {};
    double  inSumSquares[maxChannels] {};
    float   refIntervalPeak[maxChannels] {};
    float   inIntervalPeak[maxChannels] {};
    int32_t intervalClipCount { 0 };
    int32_t intervalClipCountPerChannel[maxChannels] {};
    int32_t intervalStatusBits { StatusBits::ok };
    bool    intervalDropoutSeen { false };

    // Published live values
    std::atomic<float>  atomicRefPeakDb[maxChannels];
    std::atomic<float>  atomicRefRmsDb[maxChannels];
    std::atomic<float>  atomicInPeakDb[maxChannels];
    std::atomic<float>  atomicInRmsDb[maxChannels];
    std::atomic<float>  atomicRefPeakHoldDb[maxChannels];
    std::atomic<float>  atomicInPeakHoldDb[maxChannels];

    /** Which output each input is compared against (see setReferenceOutputForInput). */
    std::atomic<int>    referenceOutputForInput[maxChannels];

    /** Per-input detection enable (see setInputMonitored). */
    std::atomic<bool>   inputMonitoredFlags[maxChannels];
    std::atomic<double> atomicElapsedSeconds { 0.0 };
};

} // namespace pnam
