/*
  ==============================================================================
    EventDetector.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "EventDetector.h"
#include <cmath>

namespace pnam
{

namespace
{
    constexpr int32_t suppressionMask = StatusBits::outputMuted
                                       | StatusBits::fadingIn
                                       | StatusBits::fadingOut
                                       | StatusBits::settling
                                       | StatusBits::inputInactive;

    inline bool isSuppressed (int32_t statusCode) noexcept
    {
        return (statusCode & suppressionMask) != 0;
    }
}

//==============================================================================
EventDetector::EventDetector (EventQueue& queueToFill)
    : eventQueue (queueToFill)
{
    thresholdSlots[0] = Thresholds{};
    thresholdSlots[1] = Thresholds{};
}

//==============================================================================
void EventDetector::prepare (double newSampleRate, int /*maximumBlockSize*/)
{
    sampleRate = newSampleRate;

    dropoutState     = ConditionState{};
    clippingState     = ConditionState{};
    levelChangeState = ConditionState{};

    baselineGainDiffDb.store (std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
    baselineRecaptureRequested.store (true, std::memory_order_relaxed);
    baselineAccumSeconds = 0.0;
    baselineAccumSum     = 0.0;
    baselineAccumBlocks  = 0;

    rateWindowStartSeconds = 0.0;
    eventsThisWindow       = 0;

    dropoutActiveAtomic.store (false, std::memory_order_relaxed);
    clippingActiveAtomic.store (false, std::memory_order_relaxed);
}

//==============================================================================
void EventDetector::setThresholds (const Thresholds& newThresholds) noexcept
{
    const int inactiveSlot = 1 - activeThresholdSlot.load (std::memory_order_relaxed);
    thresholdSlots[inactiveSlot] = newThresholds;
    activeThresholdSlot.store (inactiveSlot, std::memory_order_release);
}

EventDetector::Thresholds EventDetector::getThresholds() const noexcept
{
    return thresholdSlots[activeThresholdSlot.load (std::memory_order_acquire)];
}

void EventDetector::recaptureBaseline() noexcept
{
    baselineRecaptureRequested.store (true, std::memory_order_relaxed);
}

float EventDetector::getBaselineGainDiffDb() const noexcept
{
    return baselineGainDiffDb.load (std::memory_order_relaxed);
}

bool EventDetector::isDropoutActive() const noexcept
{
    return dropoutActiveAtomic.load (std::memory_order_relaxed);
}

bool EventDetector::isClippingActive() const noexcept
{
    return clippingActiveAtomic.load (std::memory_order_relaxed);
}

//==============================================================================
bool EventDetector::processBlockStats (const BlockStats& stats) noexcept
{
    const Thresholds t = thresholdSlots[activeThresholdSlot.load (std::memory_order_acquire)];

    const double now = stats.blockStartElapsedSeconds;
    const double blockSeconds = sampleRate > 0.0 ? (double) stats.numSamples / sampleRate : 0.0;
    const bool suppressed = isSuppressed (stats.statusCode);

    //--------------------------------------------------------------------
    // Rate limiter bookkeeping (rolling one-second window).
    if (now - rateWindowStartSeconds >= 1.0)
    {
        rateWindowStartSeconds = now;
        eventsThisWindow = 0;
    }

    auto tryPush = [this, &t] (const Event& e) noexcept -> bool
    {
        if (eventsThisWindow >= t.maxEventsPerSecond)
            return false;

        if (eventQueue.push (e))
        {
            ++eventsThisWindow;
            return true;
        }
        return false;
    };

    //--------------------------------------------------------------------
    // Baseline gain-diff capture: accumulate clean-signal gainDiff for
    // baselineCaptureMs once the settling window (handled upstream via the
    // `settling` status bit) has passed and nothing is suppressed/active.
    if (baselineRecaptureRequested.load (std::memory_order_relaxed))
    {
        if (! suppressed)
        {
            baselineAccumSeconds += blockSeconds;
            baselineAccumSum     += stats.gainDiffDb;
            ++baselineAccumBlocks;

            if (baselineAccumSeconds * 1000.0 >= t.baselineCaptureMs && baselineAccumBlocks > 0)
            {
                const float captured = (float) (baselineAccumSum / (double) baselineAccumBlocks);
                baselineGainDiffDb.store (captured, std::memory_order_relaxed);
                baselineRecaptureRequested.store (false, std::memory_order_relaxed);
                baselineAccumSeconds = 0.0;
                baselineAccumSum     = 0.0;
                baselineAccumBlocks  = 0;
            }
        }
        else
        {
            // Reset the accumulation window if suppressed mid-capture.
            baselineAccumSeconds = 0.0;
            baselineAccumSum     = 0.0;
            baselineAccumBlocks  = 0;
        }
    }

    //--------------------------------------------------------------------
    // If detection is suppressed this block, close out any active/pending
    // conditions immediately (end event where applicable) and bail early
    // for dropout/clipping/level-change (but still return current dropout
    // state, which will now be false).
    if (suppressed)
    {
        if (dropoutState.phase != ConditionState::Phase::inactive)
        {
            if (dropoutState.phase == ConditionState::Phase::active)
            {
                Event e;
                e.type          = EventType::CompleteDropout;
                e.severity      = Severity::Critical;
                e.channel       = ChannelId::Both;
                e.elapsedSeconds = now;
                e.measuredValue = now - dropoutState.conditionStartSeconds;
                e.expectedValue = 0.0;
                e.difference    = e.measuredValue;
                e.thresholdUsed = t.dropoutThresholdDb;
                e.code          = eventCodeEnd;
                tryPush (e);
            }
            dropoutState = ConditionState{};
            dropoutActiveAtomic.store (false, std::memory_order_relaxed);
        }

        if (clippingState.phase != ConditionState::Phase::inactive)
        {
            if (clippingState.phase == ConditionState::Phase::active)
            {
                Event e;
                e.type          = EventType::Clipping;
                e.severity      = (now - clippingState.conditionStartSeconds) * 1000.0 >= t.criticalClipMs
                                     ? Severity::Critical : Severity::Warning;
                e.channel       = ChannelId::Both;
                e.elapsedSeconds = now;
                e.measuredValue = now - clippingState.conditionStartSeconds;
                e.expectedValue = 0.0;
                e.difference    = (double) clippingState.accumulatedCount;
                e.thresholdUsed = t.clipThresholdLinear;
                e.code          = eventCodeEnd;
                tryPush (e);
            }
            clippingState = ConditionState{};
            clippingActiveAtomic.store (false, std::memory_order_relaxed);
        }

        if (levelChangeState.phase != ConditionState::Phase::inactive)
        {
            if (levelChangeState.phase == ConditionState::Phase::active)
            {
                Event e;
                e.type          = EventType::LevelChange;
                e.severity      = Severity::Warning;
                e.channel       = ChannelId::Both;
                e.elapsedSeconds = now;
                e.measuredValue = now - levelChangeState.conditionStartSeconds;
                e.expectedValue = 0.0;
                e.difference    = levelChangeState.worstValue;
                e.thresholdUsed = t.levelChangeToleranceDb;
                e.code          = eventCodeEnd;
                tryPush (e);
            }
            levelChangeState = ConditionState{};
        }

        return false;
    }

    //--------------------------------------------------------------------
    // Complete dropout: input RMS (max of L/R) far below the reference.
    {
        const float inRms = juce::jmax (stats.inRmsDb[0], stats.inRmsDb[1]);
        const bool conditionNow = inRms <= t.dropoutThresholdDb;

        if (conditionNow)
        {
            if (dropoutState.phase == ConditionState::Phase::inactive)
            {
                dropoutState.phase = ConditionState::Phase::pending;
                dropoutState.phaseStartSeconds = now;
                dropoutState.worstValue = inRms;
            }
            else if (dropoutState.phase == ConditionState::Phase::pending)
            {
                dropoutState.worstValue = juce::jmin (dropoutState.worstValue, (double) inRms);

                if ((now - dropoutState.phaseStartSeconds) * 1000.0 >= t.minDropoutMs)
                {
                    dropoutState.phase = ConditionState::Phase::active;
                    dropoutState.conditionStartSeconds = dropoutState.phaseStartSeconds;

                    Event e;
                    e.type          = EventType::CompleteDropout;
                    e.severity      = Severity::Critical;
                    e.channel       = ChannelId::Both;
                    e.elapsedSeconds = dropoutState.conditionStartSeconds;
                    e.measuredValue = inRms;
                    e.expectedValue = stats.refRmsDb;
                    e.difference    = (double) inRms - (double) stats.refRmsDb;
                    e.thresholdUsed = t.dropoutThresholdDb;
                    e.code          = eventCodeStart;
                    tryPush (e);
                }
            }
            else // active
            {
                dropoutState.worstValue = juce::jmin (dropoutState.worstValue, (double) inRms);
            }
        }
        else
        {
            if (dropoutState.phase == ConditionState::Phase::active)
            {
                Event e;
                e.type          = EventType::CompleteDropout;
                e.severity      = Severity::Critical;
                e.channel       = ChannelId::Both;
                e.elapsedSeconds = now;
                e.measuredValue = now - dropoutState.conditionStartSeconds;
                e.expectedValue = 0.0;
                e.difference    = e.measuredValue;
                e.thresholdUsed = t.dropoutThresholdDb;
                e.code          = eventCodeEnd;
                tryPush (e);
            }
            dropoutState = ConditionState{};
        }

        dropoutActiveAtomic.store (dropoutState.phase == ConditionState::Phase::active, std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------
    // Clipping: BlockStats::clipCount > 0 this block. A run ends after
    // clipFreeMs with no clipped samples.
    {
        const bool clippedThisBlock = stats.clipCount > 0;

        if (clippedThisBlock)
        {
            if (clippingState.phase == ConditionState::Phase::inactive)
            {
                clippingState.phase = ConditionState::Phase::active;
                clippingState.phaseStartSeconds = now;
                clippingState.conditionStartSeconds = now;
                clippingState.accumulatedCount = 0;

                Event e;
                e.type          = EventType::Clipping;
                e.severity      = Severity::Warning;
                e.channel       = ChannelId::Both;
                e.elapsedSeconds = now;
                e.measuredValue = (double) stats.clipCount;
                e.expectedValue = 0.0;
                e.difference    = (double) stats.clipCount;
                e.thresholdUsed = t.clipThresholdLinear;
                e.code          = eventCodeStart;
                tryPush (e);
            }

            clippingState.phaseStartSeconds = now; // last time clipping was observed
            clippingState.accumulatedCount += stats.clipCount;
        }
        else if (clippingState.phase == ConditionState::Phase::active)
        {
            if ((now - clippingState.phaseStartSeconds) * 1000.0 >= t.clipFreeMs)
            {
                const double duration = now - clippingState.conditionStartSeconds;

                Event e;
                e.type          = EventType::Clipping;
                e.severity      = duration * 1000.0 >= t.criticalClipMs ? Severity::Critical : Severity::Warning;
                e.channel       = ChannelId::Both;
                e.elapsedSeconds = now;
                e.measuredValue = duration;
                e.expectedValue = 0.0;
                e.difference    = (double) clippingState.accumulatedCount;
                e.thresholdUsed = t.clipThresholdLinear;
                e.code          = eventCodeEnd;
                tryPush (e);

                clippingState = ConditionState{};
            }
        }

        clippingActiveAtomic.store (clippingState.phase == ConditionState::Phase::active, std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------
    // Level change: sustained deviation of gainDiffDb from the captured
    // baseline, beyond tolerance.
    {
        const float baseline = baselineGainDiffDb.load (std::memory_order_relaxed);

        if (! std::isnan (baseline))
        {
            const double deviation = (double) stats.gainDiffDb - (double) baseline;
            const bool conditionNow = std::abs (deviation) > (double) t.levelChangeToleranceDb;

            if (conditionNow)
            {
                if (levelChangeState.phase == ConditionState::Phase::inactive)
                {
                    levelChangeState.phase = ConditionState::Phase::pending;
                    levelChangeState.phaseStartSeconds = now;
                    levelChangeState.worstValue = deviation;
                }
                else if (levelChangeState.phase == ConditionState::Phase::pending)
                {
                    if (std::abs (deviation) > std::abs (levelChangeState.worstValue))
                        levelChangeState.worstValue = deviation;

                    if ((now - levelChangeState.phaseStartSeconds) * 1000.0 >= t.levelChangeHoldMs)
                    {
                        levelChangeState.phase = ConditionState::Phase::active;
                        levelChangeState.conditionStartSeconds = levelChangeState.phaseStartSeconds;

                        Event e;
                        e.type          = EventType::LevelChange;
                        e.severity      = Severity::Warning;
                        e.channel       = ChannelId::Both;
                        e.elapsedSeconds = levelChangeState.conditionStartSeconds;
                        e.measuredValue = (double) stats.gainDiffDb;
                        e.expectedValue = (double) baseline;
                        e.difference    = deviation;
                        e.thresholdUsed = t.levelChangeToleranceDb;
                        e.code          = eventCodeStart;
                        tryPush (e);
                    }
                }
                else // active
                {
                    if (std::abs (deviation) > std::abs (levelChangeState.worstValue))
                        levelChangeState.worstValue = deviation;
                }
            }
            else
            {
                if (levelChangeState.phase == ConditionState::Phase::active)
                {
                    Event e;
                    e.type          = EventType::LevelChange;
                    e.severity      = Severity::Warning;
                    e.channel       = ChannelId::Both;
                    e.elapsedSeconds = now;
                    e.measuredValue = now - levelChangeState.conditionStartSeconds;
                    e.expectedValue = 0.0;
                    e.difference    = levelChangeState.worstValue;
                    e.thresholdUsed = t.levelChangeToleranceDb;
                    e.code          = eventCodeEnd;
                    tryPush (e);

                    // Re-arm the baseline to the new settled level.
                    recaptureBaseline();
                }
                levelChangeState = ConditionState{};
            }
        }
    }

    return dropoutActiveAtomic.load (std::memory_order_relaxed);
}

} // namespace pnam
