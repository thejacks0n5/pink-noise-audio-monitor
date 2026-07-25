#pragma once
/*
  ==============================================================================
    EventDetector.h
    Pink Noise Audio Monitor (pnam) — audio-thread threshold detection.

    Consumes the BlockStats produced by AnalysisProcessor each callback and
    pushes pnam::Event records into the EventQueue when conditions are
    detected. All checks are O(1) per block; no allocation, locks, or I/O.

    Detections (V1):
      - CompleteDropout: input RMS below dropoutThresholdDb while the
        reference is active (not muted/fading/settling), sustained for at
        least minDropoutMs. Emits ONE Event with code=eventCodeStart when the
        sustain requirement is first met, and ONE with code=eventCodeEnd
        (measuredValue = duration seconds) when input recovers. Severity:
        Critical.
      - Clipping: BlockStats::clipCount > 0. Debounced: one eventCodeStart
        when clipping begins, one eventCodeEnd (measuredValue = duration,
        difference = total clipped samples) after clipFreeMs without clipped
        samples. Severity: Warning (Critical if the clip run exceeds
        criticalClipMs).
      - LevelChange: |gainDiffDb - baselineGainDiffDb| > levelChangeToleranceDb
        sustained for levelChangeHoldMs. The baseline round-trip gain is
        captured automatically after the settling window (first
        baselineCaptureMs of clean signal); an eventCodeStart fires when the
        deviation begins, eventCodeEnd when it returns within tolerance, after
        which the baseline is re-armed to the new level. Severity: Warning.

    Debounce / rate-limit strategy: every condition is a state machine
    (Inactive -> Pending -> Active) so an hours-long dropout produces exactly
    two Event records, never thousands. Global belt-and-braces limiter:
    at most maxEventsPerSecond pushes per second (excess transitions are
    counted and reported in the next end-event's `code`-adjacent fields by the
    implementation; the queue also has its own drop counter).

    Checks are suppressed while BlockStats::statusCode has any of
    outputMuted | fadingIn | fadingOut | settling | inputInactive set;
    ongoing conditions are closed out (end event) when suppression begins.
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
        float  dropoutThresholdDb     { -60.0f };  ///< Input RMS below this (dBFS) counts as dropout.
        double minDropoutMs           { 50.0 };    ///< Sustain required before a dropout Event fires.
        float  clipThresholdLinear    { 0.999f };  ///< |sample| >= this counts as clipped.
        double clipFreeMs             { 250.0 };   ///< Clip-free time that ends a clipping run.
        double criticalClipMs         { 1000.0 };  ///< Clip run longer than this escalates to Critical.
        float  levelChangeToleranceDb { 6.0f };    ///< Allowed |deviation| from baseline gain diff.
        double levelChangeHoldMs      { 500.0 };   ///< Sustain required before a LevelChange fires.
        double baselineCaptureMs      { 2000.0 };  ///< Clean-signal time used to capture baseline gain.
        double settlingMs             { 3000.0 };  ///< Post-start warm-up during which detection is off.
        int    maxEventsPerSecond     { 20 };      ///< Global push rate limiter.
    };

    static_assert (std::is_trivially_copyable<Thresholds>::value,
                   "Thresholds crosses threads by value");

    //==========================================================================
    /** @param queueToFill  the audio->message event FIFO. Must outlive this. */
    explicit EventDetector (EventQueue& queueToFill);

    EventDetector (const EventDetector&) = delete;
    EventDetector& operator= (const EventDetector&) = delete;

    /** NON-REAL-TIME, audio stopped. Resets all state machines, the settling
        timer, and the captured baseline. */
    void prepare (double sampleRate, int maximumBlockSize);

    /** MESSAGE THREAD. Replaces the full threshold set (double-buffered
        internally; the audio thread adopts it at the next block boundary). */
    void setThresholds (const Thresholds& newThresholds) noexcept;

    /** Copy of the thresholds currently in effect. Any thread. */
    Thresholds getThresholds() const noexcept;

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
    float getBaselineGainDiffDb() const noexcept;

    /** MESSAGE THREAD. Re-arm baseline capture (e.g. after the user changes
        levels deliberately). Takes effect at the next block. */
    void recaptureBaseline() noexcept;

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
    };

    ConditionState dropoutState, clippingState, levelChangeState;

    // Baseline capture
    std::atomic<float> baselineGainDiffDb { std::numeric_limits<float>::quiet_NaN() };
    std::atomic<bool>  baselineRecaptureRequested { true };
    double             baselineAccumSeconds { 0.0 };
    double             baselineAccumSum     { 0.0 };
    int64_t            baselineAccumBlocks  { 0 };

    // Threshold double-buffer (message thread writes inactive slot, flips index)
    Thresholds          thresholdSlots[2];
    std::atomic<int>    activeThresholdSlot { 0 };

    // Rate limiter
    double  rateWindowStartSeconds { 0.0 };
    int     eventsThisWindow       { 0 };

    // Published flags
    std::atomic<bool> dropoutActiveAtomic  { false };
    std::atomic<bool> clippingActiveAtomic { false };

    double sampleRate { 0.0 };
};

} // namespace pnam
