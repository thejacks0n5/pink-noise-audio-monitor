#pragma once
/*
  ==============================================================================
    EventDetector.h
    Pink Noise Audio Monitor (pnam) — audio-thread threshold detection.

    Consumes the BlockStats produced by AnalysisProcessor each callback and
    pushes pnam::Event records into the EventQueue when conditions are
    detected. All checks are O(1) per block; no allocation, locks, or I/O.

    Detections (V1):
      - CompleteDropout: block input RMS at/below the ENTRY LEVEL while the
        reference is active (not muted/fading/settling), sustained for at
        least minDropoutMs. Once a baseline exists the entry level is purely
        relative: dropoutRelativeDb below the level the reference plus the
        captured baseline gain predicts. The absolute floor
        (dropoutThresholdDb) has no say then — with the output turned down a
        healthy return sits near or below -60 dBFS and ordinary block-to-block
        scatter of pink noise crosses any fixed floor, while at normal levels
        a pulled cable leaves crosstalk well above it. Before a baseline
        exists (the first seconds after a start) the loop gain is unknown, so
        the reference is the yardstick: the entry level is the LOWER of the
        absolute floor and "dropoutRelativeDb below the reference" — a dead
        input at normal output level is still caught as soon as settling ends,
        and a healthy return at a low output level is not called dead.
        The prediction uses the LOWER of the smoothed reference and this
        block's reference, so it follows a deliberate output-level reduction
        at once instead of calling the quieter return a dropout.
        There is hysteresis both ways: once the input has touched the entry
        level only rising above the EXIT LEVEL cancels a pending dropout, and
        an active one ends only once the input has stayed above the exit level
        for minDropoutMs. The exit level is 6 dB above the entry level, capped
        at half the distance from the entry level to the input's normal level
        — where the normal level is only a few dB above the entry level (a
        quiet loop judged before it has a baseline) a fixed 6 dB would be a
        level the input never reaches and the dropout would never end; in that
        squeezed case "above" is judged on the input averaged over about
        minDropoutMs rather than on single blocks. Emits ONE Event
        with code=eventCodeStart when the sustain requirement is first met, and
        ONE with code=eventCodeEnd (measuredValue = duration seconds, stamped
        at the moment the input came back) when it has recovered. Severity:
        Critical.
      - Clipping: BlockStats::clipCount > 0. Debounced: one eventCodeStart
        when clipping begins, one eventCodeEnd (measuredValue = duration,
        difference = total clipped samples) after clipFreeMs without clipped
        samples. Severity: Warning (Critical if the clip run exceeds
        criticalClipMs).
      - LevelChange: |smoothed gain - baselineGainDiffDb| >
        levelChangeToleranceDb sustained for levelChangeHoldMs, where the
        smoothed gain is the ratio of one-pole-smoothed (~400 ms) input and
        reference energies. Raw per-block RMS cannot be used: the returned
        block lags the reference block by the round-trip latency, so their
        per-block ratio scatters by several dB and a real 8-15 dB change was
        missed or "ended" within a fraction of a second. The baseline
        round-trip gain is captured automatically after the settling window
        (first baselineCaptureMs of clean signal); an eventCodeStart fires
        when the deviation begins, eventCodeEnd once it has been back within
        tolerance for levelChangeHoldMs, after which the baseline is re-armed
        to the new level. A channel whose level change is active keeps its
        baseline across a device restart (see prepare()). Not evaluated on a channel that is in (or possibly
        entering) a dropout: that fault is already reported once, as the
        dropout. Severity: Warning.

    Debounce / rate-limit strategy: every condition is a state machine
    (Inactive -> Pending -> Active) so an hours-long dropout produces exactly
    two Event records, never thousands. Global belt-and-braces limiter: at
    most maxEventsPerSecond START events per second. Nothing is ever dropped
    by it or by a full queue: a Start that cannot be pushed is kept in its
    state machine and retried every block, and an End is never rate-limited
    but always waits for its own Start, so events.csv cannot contain a start
    without an end or an end without a start.

    Checks are suppressed while BlockStats::statusCode has any of
    outputMuted | fadingIn | fadingOut | settling | inputInactive set. Conditions
    already ACTIVE are carried across the suppression window rather than closed
    out: one unresolved fault stays one event until it genuinely clears, instead
    of being re-reported after every device restart. Pending (unconfirmed)
    conditions are discarded. Inputs whose BlockStats::inputMonitored is false
    are skipped entirely.
  ==============================================================================
*/

#include <atomic>
#include <cstdint>
#include <limits>
#include <juce_core/juce_core.h>

#include "../Model/DataModel.h"
#include "MeasurementQueue.h"
#include "AnalysisProcessor.h"

namespace pnam
{

//==============================================================================
class EventDetector
{
public:
    //==========================================================================
    /** All tunable thresholds. POD; set atomically as a whole via
        setThresholds() from the message thread. */
    struct Thresholds
    {
        float  dropoutThresholdDb     { -60.0f };  ///< Absolute floor (dBFS). Only used until a baseline has been captured.
        double minDropoutMs           { 50.0 };    ///< Sustain required before a dropout fires, and before it ends.
        float  clipThresholdLinear    { 0.999f };  ///< |sample| >= this counts as clipped.
        double clipFreeMs             { 250.0 };   ///< Clip-free time that ends a clipping run.
        double criticalClipMs         { 1000.0 };  ///< Clip run longer than this escalates to Critical.
        float  levelChangeToleranceDb { 6.0f };    ///< Allowed |deviation| from baseline gain diff.
        double levelChangeHoldMs      { 500.0 };   ///< Sustain required before a LevelChange fires.
        double baselineCaptureMs      { 2000.0 };  ///< Clean-signal time used to capture baseline gain.
        double settlingMs             { 3000.0 };  ///< Post-start warm-up during which detection is off.
        int    maxEventsPerSecond     { 20 };      ///< Global rate limiter on Start events (deferred, never dropped).
        float  dropoutRelativeDb      { 20.0f };   ///< Input this far below its predicted level counts as dropout.
    };

    static_assert (std::is_trivially_copyable<Thresholds>::value,
                   "Thresholds crosses threads by value");

    //==========================================================================
    /** @param queueToFill  the audio->message event FIFO. Must outlive this. */
    explicit EventDetector (EventQueue& queueToFill);

    EventDetector (const EventDetector&) = delete;
    EventDetector& operator= (const EventDetector&) = delete;

    /** Called from audioDeviceAboutToStart on every device (re)start. Adopts the
        new sample rate, re-arms the baseline and resets the rate-limit window,
        but deliberately PRESERVES active conditions: a device restart must not
        silently discard an ongoing fault, which would strand its start event
        with no end and then re-report it as a new fault. For the same reason a
        channel with an ACTIVE level change keeps its baseline: recapturing it
        would measure the faulty level, call that normal, and end the fault. */
    void prepare (double sampleRate, int maximumBlockSize);

    /** MESSAGE THREAD, callback detached. A genuine new session: wipes every
        state machine, baseline and rate-limit counter. Must be called by
        AudioEngine::start(), which zeroes the session clock at the same time —
        without it, a stopped-and-restarted session inherits a rate-limit window
        set in the future and silently drops every event until the clock catches
        up. */
    void resetSessionState() noexcept;

    /** MESSAGE THREAD. Replaces the full threshold set (double-buffered
        internally; the audio thread adopts it at the next block boundary). */
    void setThresholds (const Thresholds& newThresholds) noexcept;

    /** Copy of the thresholds currently in effect. MESSAGE THREAD: retries
        until it reads a stable snapshot, so it must not be called from the
        audio callback. */
    Thresholds getThresholds() const noexcept;

    /** Settling window (ms) from the most recent setThresholds(). Wait-free, any
        thread: this is what the audio callback reads instead of getThresholds(). */
    double getSettlingMs() const noexcept
    {
        return settlingMsAtomic.load (std::memory_order_relaxed);
    }

    //==========================================================================
    /** AUDIO THREAD. Run all detections against one block's stats. Call once
        per callback, right after AnalysisProcessor::processBlock.
        @returns true if a dropout condition is currently active — the
        AudioEngine forwards that to AnalysisProcessor::noteDropoutActive(). */
    bool processBlockStats (const BlockStats& stats) noexcept;

    /** True while a dropout condition is active (atomic; UI may poll). */
    bool isDropoutActive() const noexcept;

    /** True while a clipping run is active (atomic; UI may poll). */
    bool isClippingActive() const noexcept;

    /** Baseline round-trip gain (dB) captured after settling; NaN until
        captured. Atomic; UI display only. */
    /** Captured round-trip baseline for one input channel, or NaN if not yet
        captured. Each input has its own, since each references its own output. */
    float getBaselineGainDiffDb (int inputChannel = 0) const noexcept;

    /** MESSAGE THREAD. Re-arm baseline capture on every channel (e.g. after the
        user changes levels deliberately). Takes effect at the next block. */
    void recaptureBaseline() noexcept;

    /** ANY THREAD. Re-arm baseline capture for one input channel only — used
        when that channel's level change ends, so other channels' baselines are
        left untouched. */
    void recaptureBaseline (int inputChannel) noexcept;

private:
    //==========================================================================
    EventQueue& eventQueue;

    /** Per-condition debounce state machine. */
    struct ConditionState
    {
        enum class Phase : int { inactive, pending, active };
        Phase   phase { Phase::inactive };
        double  phaseStartSeconds { 0.0 };  ///< When the current phase began.
        double  conditionStartSeconds { 0.0 };
        double  worstValue { 0.0 };
        int64_t accumulatedCount { 0 };     ///< e.g. total clipped samples in a run.

        // Active only. The Start event is built once, when the condition is
        // confirmed, and kept here until the queue has accepted it; the End is
        // never pushed before it. That is what keeps start/end pairs matched
        // when the rate limiter or a full queue holds a push back.
        Event   startEvent {};
        bool    startReported { false };

        // Active only: exit hold. The condition has to stay clear continuously
        // from clearStartSeconds before the End is emitted.
        bool    clearing { false };
        double  clearStartSeconds { 0.0 };

        // Dropout only: the entry and exit levels (dBFS) in force for this
        // dropout, kept so a device restart that discards the baseline cannot
        // end it by falling back to the lower absolute floor.
        double  entryLevelDb { 0.0 };
        double  exitLevelDb  { 0.0 };

        // Dropout only: input mean-square averaged over roughly minDropoutMs,
        // started from the block that began this dropout (so it carries nothing
        // from before it). A second way of seeing that the input is back, for
        // when single blocks scatter too much to all clear the exit level.
        double  recentMeanSquare { 0.0 };
    };

    // One independent state machine per input channel, so a fault on one input
    // neither masks nor duplicates a fault on another.
    ConditionState dropoutState[maxChannels], clippingState[maxChannels],
                   levelChangeState[maxChannels];

    // Baseline capture, per input channel (each input has its own round-trip
    // gain against whichever output it references).
    std::atomic<float> baselineGainDiffDb[maxChannels];

    // Per-channel recapture. A single global flag meant that when one channel's
    // level change ended and re-armed the baseline, every OTHER channel's
    // current level — however wrong — was baked into its new baseline too,
    // silently ending any deviation it was tracking.
    std::atomic<bool>  baselineRecaptureRequested[maxChannels];
    double             baselineAccumSeconds[maxChannels] {};
    double             baselineAccumSum[maxChannels] {};
    int64_t            baselineAccumBlocks[maxChannels] {};

    // One-pole-smoothed mean-square per output (reference) and per input
    // channel. Audio-thread state. smoothedBlockCount is the number of blocks
    // folded in since smoothing last (re)started; while it is small the
    // smoother is a plain running mean, so it starts converged instead of
    // seeded with one noisy block.
    double             smoothedRefMeanSquare[maxChannels] {};
    double             smoothedInMeanSquare[maxChannels] {};
    int64_t            smoothedBlockCount { 0 };

    // Threshold hand-off: a seqlock. The old two-slot flip could tear — two
    // rapid setThresholds() calls could write the slot the audio thread was
    // still copying from. The audio thread now keeps its own copy and only
    // adopts the shared data when the sequence is even and unchanged across the
    // read, so it is wait-free and can never observe a torn write.
    Thresholds              thresholdShared;
    std::atomic<uint32_t>   thresholdSeq { 0 };
    Thresholds              currentThresholds;   // audio-thread-owned copy

    // Mirror of thresholdShared.settlingMs for the audio callback, which must
    // not run the retrying seqlock read in getThresholds().
    std::atomic<double>     settlingMsAtomic { 3000.0 };

    // Rate limiter
    double  rateWindowStartSeconds { 0.0 };
    int     eventsThisWindow       { 0 };

    // Published flags — true when ANY input channel has the condition active.
    std::atomic<bool> dropoutActiveAtomic  { false };
    std::atomic<bool> clippingActiveAtomic { false };

    double sampleRate { 0.0 };
};

} // namespace pnam
