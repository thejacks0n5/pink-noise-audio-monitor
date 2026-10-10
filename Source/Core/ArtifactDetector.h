#pragma once
/*
  ==============================================================================
    ArtifactDetector.h
    Pink Noise Audio Monitor (pnam) — clicks, noise, distortion, timing slips.

    The level-based detectors (EventDetector) can only say "the return got
    quieter / louder / silent". This one answers "is what came back still the
    signal that was sent?". Because the reference is known exactly, anything in
    the return that is not a delayed, filtered copy of it is an artifact.

    Per monitored input, on the audio thread:

      1. ALIGN. The round-trip delay is found by cross-correlating the return
         against the reference (GCC-PHAT, computed off the audio thread from a
         snapshot the audio thread captures), then tracked.
      2. MODEL. A short NLMS filter learns the path's linear response (gain,
         polarity, fractional delay, gentle EQ) and predicts the return from
         the reference.
      3. RESIDUAL. return - prediction is everything the path ADDED. Its level
         relative to the prediction is logged every interval ("residual", a
         THD+N-like figure), and tested for:
           - Click      : an isolated residual sample far outside its own
                          running spread (click, pop, tick).
           - Noise      : residual risen well above its learned baseline and
                          spread evenly over time (static, hiss, crackle, buzz,
                          interference).
           - Distortion : the same rise, but concentrated on the signal's own
                          peaks (clipping or saturation somewhere in the path).
           - TimingSlip : the return stopped matching and matched again at a
                          DIFFERENT delay: samples were dropped or repeated.
           - SignalMismatch : the return is present but is not the reference
                          (wrong routing, another source, or destroyed audio).

    Both signals are high-passed identically before comparison (analysisHpHz).
    AC coupling in real converters shifts phase at the bottom of the band where
    pink noise carries most of its energy, and a short filter cannot model
    that; removing it buys tens of dB of residual floor. The cost, stated
    plainly: an artifact that lives ONLY below that corner — pure mains hum at
    50/60 Hz, a slow thump — is attenuated here and may not be reported.

    Real-time rules: processBlock() never allocates, locks or blocks. All
    storage is sized in prepare(). The cross-correlation runs in
    serviceAcquisitions(), which the owner calls from a background thread; the
    two sides hand a capture buffer back and forth through one atomic per
    channel.
  ==============================================================================
*/

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include "../Model/DataModel.h"
#include "MeasurementQueue.h"

namespace pnam
{

//==============================================================================
class ArtifactDetector
{
public:
    /** Tunables. Trivially copyable; handed to the audio thread through
        atomics, one field at a time (each is independently meaningful). */
    struct Settings
    {
        bool   enabled               { true };
        float  clickSigma            { 8.0f };   ///< Residual sample must exceed this many running sigmas...
        float  clickMinRelDb         { -40.0f }; ///< ...and be at least this high relative to the predicted signal RMS.
        float  clickEventRelDb       { -20.0f }; ///< A click this large (re signal RMS) is an event of its own; smaller ones
                                                 ///< are counted every interval and summarised now and then.
        float  noiseRiseDb           { 10.0f };  ///< Residual rise over baseline that counts as a fault.
        float  noiseMinRatioDb       { -50.0f }; ///< Never report a residual below this, however clean the baseline.
        double noiseHoldMs           { 200.0 };  ///< Sustain before a Noise/Distortion event starts.
        double noiseEndHoldMs        { 500.0 };  ///< Clean time before it ends.
        float  distortionPeakDb      { 6.0f };   ///< Residual this much stronger on signal peaks => Distortion.
    };

    static_assert (std::is_trivially_copyable<Settings>::value, "Settings crosses threads by value");

    /** Number of taps in the path model. */
    static constexpr int numTaps = 32;

    /** A slip of up to this many samples either way is recognised and fitted
        on the spot; larger ones are found by searching again. */
    static constexpr int maxTrialShift = 4;
    static constexpr int numShiftTrials = 2 * maxTrialShift;

    explicit ArtifactDetector (EventQueue& queueToFill);
    ~ArtifactDetector();

    ArtifactDetector (const ArtifactDetector&) = delete;
    ArtifactDetector& operator= (const ArtifactDetector&) = delete;

    //==========================================================================
    /** NON-REAL-TIME, callback detached. Sizes every buffer for this device
        configuration and drops alignment (the path may have changed). Active
        conditions are kept, exactly as EventDetector::prepare() keeps them. */
    void prepare (double sampleRate, int maximumBlockSize, int numInputChannels, int numOutputChannels);

    /** NON-REAL-TIME, callback detached. A genuine new session. */
    void resetSessionState() noexcept;

    void setSettings (const Settings&) noexcept;
    Settings getSettings() const noexcept;

    /** ANY THREAD. Which output an input is compared against, and whether the
        input takes part at all. Mirrors AnalysisProcessor's mapping. */
    void setReferenceOutputForInput (int inputChannel, int outputChannel) noexcept;
    void setInputMonitored (int inputChannel, bool monitored) noexcept;

    /** ANY THREAD. Re-learn every channel's residual baseline. */
    void recaptureBaseline() noexcept;

    //==========================================================================
    /** AUDIO THREAD. Call once per callback.
        @param input        the returned input block
        @param reference    the block just sent to the outputs (post gain/fade)
        @param statusBits   StatusBits for this block; detection is suspended
                            while muted, fading, settling or input-inactive
        @param blockStartSeconds  session clock at the first sample */
    void processBlock (const float* const* input, int numInputChannels,
                       const float* const* reference, int numReferenceChannels,
                       int numSamples, int32_t statusBits, double blockStartSeconds) noexcept;

    //==========================================================================
    /** NON-REAL-TIME (background thread). Runs any cross-correlation the audio
        thread has captured data for. Returns true if it did some work. */
    bool serviceAcquisitions();

    //==========================================================================
    /** Per-input figures for the measurement row. AUDIO THREAD: takes the
        interval's accumulators and clears them. */
    struct IntervalFigures
    {
        float   residualDb  { silenceFloorDb };  ///< Residual relative to predicted signal, dB. Valid only if locked.
        float   latencyMs   { 0.0f };            ///< Round-trip latency. Valid only if locked.
        int32_t clickCount  { 0 };
        bool    locked      { false };
    };

    IntervalFigures takeIntervalFigures (int inputChannel) noexcept;

    /** ANY THREAD (atomics) — for the UI. */
    bool  isLocked (int inputChannel) const noexcept;

    /** True once the alignment has been verified (the model converged on it),
        which is when getLatencyMs() is a measurement rather than a candidate. */
    bool  isAligned (int inputChannel) const noexcept;
    float getLatencyMs (int inputChannel) const noexcept;
    float getResidualDb (int inputChannel) const noexcept;

private:
    //==========================================================================
    struct Biquad
    {
        double b0 { 1.0 }, b1 { 0.0 }, b2 { 0.0 }, a1 { 0.0 }, a2 { 0.0 };
        double z1 { 0.0 }, z2 { 0.0 };

        void setHighPass (double sampleRate, double frequency) noexcept;
        void setLowPass (double sampleRate, double frequency, double q) noexcept;
        void reset() noexcept { z1 = z2 = 0.0; }

        inline float process (float x) noexcept
        {
            const double y = b0 * (double) x + z1;
            z1 = b1 * (double) x - a1 * y + z2;
            z2 = b2 * (double) x - a2 * y;
            return (float) y;
        }
    };

    /** The band the comparison is made in: both the reference and the return
        go through an identical copy of this. */
    struct AnalysisFilter
    {
        Biquad highPass, lowPassA, lowPassB;

        void prepare (double sampleRate) noexcept;
        void reset() noexcept   { highPass.reset(); lowPassA.reset(); lowPassB.reset(); }
        inline float process (float x) noexcept   { return lowPassB.process (lowPassA.process (highPass.process (x))); }
    };

    struct Condition
    {
        enum class Phase : int { inactive, pending, active };
        Phase  phase { Phase::inactive };
        double phaseStartSeconds { 0.0 };
        double conditionStartSeconds { 0.0 };
        double clearSinceSeconds { -1.0 };
        double worstValue { 0.0 };
        bool   startReported { false };
    };

    enum class AcqState : int { idle, capturing, captured, done };

    struct Channel
    {
        // --- path model -----------------------------------------------------
        bool    retired { true };            ///< not being processed (unmonitored / off): model forgotten
        int     activeReference { -1 };      ///< the output this model was fitted against
        bool    discardAcquisition { false };
        bool    locked { false };
        bool    lockVerified { false };      ///< model converged on this alignment
        bool    everLocked { false };
        int     delay { 0 };                 ///< delay of the newest tap, samples
        float   weights[numTaps] {};
        // One more regressor: a leaky integral of the reference. AC coupling
        // (a first-order high-pass far below the band) leaves a phase lead of
        // about fc/f, which is exactly "minus a little of the integral" — one
        // coefficient here cancels what 32 taps cannot reach.
        double  integral { 0.0 }, integralPower { 0.0 };
        float   integralWeight { 0.0f };
        AnalysisFilter inputFilter;
        double  lockedAtSeconds { 0.0 };
        double  stableDelay { 0.0 };         ///< effective delay while the residual was last normal
        bool    delayFrozen { false };       ///< residual elevated: stableDelay held for comparison
        double  delayFrozenSeconds { 0.0 };
        bool    slipCheckPending { false };  ///< lost the return; classify once it is found again
        double  lossSeconds { 0.0 };
        double  unlockedSinceSeconds { -1.0 };
        double  highResidualSinceSeconds { -1.0 };

        // --- running statistics --------------------------------------------
        double  fastResidualPower { 0.0 }, fastPredictedPower { 0.0 }, fastInputPower { 0.0 };
        double  midResidualPower { 0.0 }, midPredictedPower { 0.0 };
        double  sampleInputPower { 0.0 };    ///< ~1 ms return power, for the per-sample absent gate
        double  residualSpread { 0.0 };      ///< running mean |residual[n] - residual[n-1]|
        double  previousError { 0.0 };
        double  rawInputPower { 0.0 };       ///< full-band input mean-square
        double  referencePower { 0.0 };      ///< long-run mean-square of the (band-limited) reference
        double  peakResidualPower { 0.0 }, offPeakResidualPower { 0.0 };
        // A run of residual outliers, carried across blocks until it is known
        // what it was: a click (it stopped), a change in the path's gain (the
        // error is the signal itself, scaled), or the start of something longer.
        bool    runActive { false };
        bool    runWaitForQuiet { false };
        int     runSamples { 0 }, runSinceOutlier { 0 }, quietSamples { 0 };
        double  runStartSeconds { 0.0 }, runPeak { 0.0 };
        double  runErrorPredicted { 0.0 }, runErrorPower { 0.0 }, runPredictedPower { 0.0 };
        double  runShiftErrorPower[numShiftTrials] {};
        bool    smallSlipPending { false };
        double  smallSlipSeconds { 0.0 };
        bool    pendingClickValid { false };
        double  pendingClickSeconds { 0.0 }, pendingClickPeak { 0.0 }, pendingClickDuration { 0.0 }, pendingClickThreshold { 0.0 };
        double  pendingClickSignalRms { 0.0 };
        // Small clicks: real, but too small to be worth an event each (a rig
        // can tick faintly every couple of minutes all night).
        int     smallClicksSinceReport { 0 };
        double  smallClickLargestPeak { 0.0 }, smallClickLargestDuration { 0.0 };
        double  lastSmallClickReportSeconds { -1.0e9 };
        double  clickRate { 0.0 };           ///< decaying count of recent clicks
        double  lastClickSeconds { -1.0e9 };
        double  ratioClearSinceSeconds { -1.0 };
        int     absentCooldownSamples { 0 }; ///< no click can start this soon after a dropout
        int     linearHoldSamples { 0 };     ///< detection paused while the model re-learns a new gain
        bool    burstPending { false };      ///< longer than a click, cause not yet known
        double  burstStartSeconds { 0.0 }, burstPeak { 0.0 };
        double  clickWindowStartSeconds { 0.0 };
        int     clickEventsThisWindow { 0 };

        // --- baseline -------------------------------------------------------
        float   baselineDb { 0.0f };
        float   baselineCandidate { 0.0f };
        double  baselinePredictedPower { 0.0 };  ///< signal level the baseline was learned at
        float   settledRatioDb { 0.0f };     ///< slow average of the residual while nothing is wrong
        int     baselineSegment { 0 };
        bool    baselineValid { false };
        double  baselineSum { 0.0 };
        int     baselineBlocks { 0 };
        double  baselineStartSeconds { 0.0 };
        std::atomic<bool> baselineRecaptureRequested { false };

        // --- conditions -----------------------------------------------------
        Condition noise;                     ///< Noise or Distortion (decided at start)
        EventType noiseType { EventType::Noise };
        Condition mismatch;

        // --- interval accumulators -----------------------------------------
        double  intervalResidual { 0.0 }, intervalPredicted { 0.0 };
        int32_t intervalClicks { 0 };

        // --- acquisition hand-off ------------------------------------------
        std::atomic<int>  acqState { (int) AcqState::idle };
        std::vector<float> captureReference, captureInput;
        int     captureTarget { 0 };         ///< samples (decimated) wanted
        int     captureCount { 0 };
        int     captureDelayBase { 0 };      ///< reference was captured this many samples late
        int     decimPhase { 0 };
        float   decimRefAcc { 0.0f }, decimInAcc { 0.0f };
        bool    lastAcquisitionWasShort { false };
        int     resultLag { 0 };             ///< decimated samples
        float   resultConfidence { 0.0f };

        // --- published ------------------------------------------------------
        std::atomic<bool>  lockedAtomic { false };
        std::atomic<bool>  alignedAtomic { false };
        std::atomic<float> latencyMsAtomic { 0.0f };
        std::atomic<float> residualDbAtomic { silenceFloorDb };

        std::atomic<int>  referenceOutput { 0 };
        std::atomic<bool> monitored { true };
    };

    //==========================================================================
    void processChannel (int ch, const float* input, int numSamples, bool suppressed,
                         double blockStartSeconds, const Settings&) noexcept;

    void requestAcquisition (Channel&, bool shortSearch) noexcept;
    void feedCapture (Channel&, const float* filteredInput, int referenceChannel, int numSamples) noexcept;
    void adoptAcquisitionResult (int ch, Channel&, double now) noexcept;
    void dropLock (int ch, Channel&, double now, bool reportable) noexcept;
    void recentre (Channel&) noexcept;
    void resetModel (Channel&) noexcept;
    void retire (int ch, Channel&, double now) noexcept;
    void reportClick (int ch, Channel&, double seconds, double peak, double durationSeconds, double threshold) noexcept;
    void flushPendingClick (int ch, Channel&) noexcept;
    double effectiveDelay (const Channel&) const noexcept;

    void readFilteredReference (int referenceChannel, int delaySamples, int numSamples, float* dest) const noexcept;

    bool pushEvent (const Event&) noexcept;
    Event makeEvent (EventType, Severity, int ch, double seconds, int32_t code) const noexcept;

    Settings loadSettings() const noexcept;

    //==========================================================================
    EventQueue& eventQueue;

    double sampleRate { 0.0 };
    int    maxBlock { 0 };
    int    numIn { 0 }, numOut { 0 };
    int    decimation { 1 };
    int    fullCaptureLength { 0 }, shortCaptureLength { 0 };

    std::unique_ptr<Channel[]> channels;      // maxChannels entries, allocated once

    // High-passed reference, one ring per output channel.
    std::vector<std::vector<float>> referenceRing;
    AnalysisFilter referenceFilter[maxChannels];
    int     ringMask { 0 };
    int64_t ringWritten { 0 };

    std::vector<float> scratchInput, scratchReference;

    // Settings, one atomic per field.
    std::atomic<bool>   sEnabled { true };
    std::atomic<float>  sClickSigma, sClickMinRelDb, sClickEventRelDb, sNoiseRiseDb, sNoiseMinRatioDb, sDistortionPeakDb;
    std::atomic<double> sNoiseHoldMs, sNoiseEndHoldMs;

    // Rate limiter (shared by every channel; End events are never limited).
    double rateWindowStartSeconds { 0.0 };
    int    eventsThisWindow { 0 };

    // Background-thread scratch for the cross-correlation.
    // (prepare() and serviceAcquisitions() are both non-real-time and exclude
    // each other with serviceLock; the audio thread never takes it.)
    juce::CriticalSection serviceLock;
    std::unique_ptr<juce::dsp::FFT> fftFull, fftShort;
    int fullOrder { 0 }, shortOrder { 0 };
    std::vector<float> fftA, fftB;
};

} // namespace pnam
