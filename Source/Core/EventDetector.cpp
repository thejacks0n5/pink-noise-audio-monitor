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
    thresholdShared   = Thresholds{};
    currentThresholds = Thresholds{};

    // Value-initialised atomics would read 0.0, not NaN, breaking the
    // "no baseline captured yet" contract for anything polling before start.
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        baselineGainDiffDb[ch].store (std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
        baselineRecaptureRequested[ch].store (true, std::memory_order_relaxed);
    }
}

//==============================================================================
void EventDetector::prepare (double newSampleRate, int /*maximumBlockSize*/)
{
    sampleRate = newSampleRate;

    // Active conditions survive: a reconfiguration is not a reason to forget an
    // ongoing fault. Their timestamps travel with them, so the eventual end
    // event still measures from the original start. Pending (unconfirmed) ones
    // are dropped — see the class comment.
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        if (dropoutState[ch].phase == ConditionState::Phase::pending)
            dropoutState[ch] = ConditionState{};

        if (clippingState[ch].phase == ConditionState::Phase::pending)
            clippingState[ch] = ConditionState{};

        if (levelChangeState[ch].phase == ConditionState::Phase::pending)
            levelChangeState[ch] = ConditionState{};

        // The round-trip gain is device-dependent, so it must be recaptured.
        baselineGainDiffDb[ch].store (std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
        baselineRecaptureRequested[ch].store (true, std::memory_order_relaxed);
        baselineAccumSeconds[ch] = 0.0;
        baselineAccumSum[ch]     = 0.0;
        baselineAccumBlocks[ch]  = 0;
    }

    // The clock may have moved; a stale window would gate events off.
    rateWindowStartSeconds = 0.0;
    eventsThisWindow       = 0;
}

void EventDetector::resetSessionState() noexcept
{
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        dropoutState[ch]     = ConditionState{};
        clippingState[ch]    = ConditionState{};
        levelChangeState[ch] = ConditionState{};

        baselineGainDiffDb[ch].store (std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
        baselineRecaptureRequested[ch].store (true, std::memory_order_relaxed);
        baselineAccumSeconds[ch] = 0.0;
        baselineAccumSum[ch]     = 0.0;
        baselineAccumBlocks[ch]  = 0;
    }

    rateWindowStartSeconds = 0.0;
    eventsThisWindow       = 0;

    dropoutActiveAtomic.store (false, std::memory_order_relaxed);
    clippingActiveAtomic.store (false, std::memory_order_relaxed);
}

//==============================================================================
void EventDetector::setThresholds (const Thresholds& newThresholds) noexcept
{
    // Seqlock write: odd sequence = write in progress.
    const auto seq = thresholdSeq.load (std::memory_order_relaxed);
    thresholdSeq.store (seq + 1, std::memory_order_release);
    std::atomic_thread_fence (std::memory_order_release);
    thresholdShared = newThresholds;
    std::atomic_thread_fence (std::memory_order_release);
    thresholdSeq.store (seq + 2, std::memory_order_release);
}

EventDetector::Thresholds EventDetector::getThresholds() const noexcept
{
    // Message-thread readers can afford to retry until a stable read.
    for (;;)
    {
        const auto s1 = thresholdSeq.load (std::memory_order_acquire);
        if ((s1 & 1u) != 0)
            continue;

        Thresholds copy = thresholdShared;
        std::atomic_thread_fence (std::memory_order_acquire);

        if (thresholdSeq.load (std::memory_order_relaxed) == s1)
            return copy;
    }
}

void EventDetector::recaptureBaseline() noexcept
{
    for (int ch = 0; ch < maxChannels; ++ch)
        baselineRecaptureRequested[ch].store (true, std::memory_order_relaxed);
}

void EventDetector::recaptureBaseline (int inputChannel) noexcept
{
    baselineRecaptureRequested[juce::jlimit (0, maxChannels - 1, inputChannel)]
        .store (true, std::memory_order_relaxed);
}

float EventDetector::getBaselineGainDiffDb (int inputChannel) const noexcept
{
    return baselineGainDiffDb[juce::jlimit (0, maxChannels - 1, inputChannel)]
             .load (std::memory_order_relaxed);
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
    // Adopt new thresholds only when a stable (even, unchanged) snapshot is
    // available; otherwise keep using the current copy. Wait-free.
    {
        const auto s1 = thresholdSeq.load (std::memory_order_acquire);

        if ((s1 & 1u) == 0)
        {
            const Thresholds candidate = thresholdShared;
            std::atomic_thread_fence (std::memory_order_acquire);

            if (thresholdSeq.load (std::memory_order_relaxed) == s1)
                currentThresholds = candidate;
        }
    }

    const Thresholds& t = currentThresholds;

    const double now = stats.blockStartElapsedSeconds;

    // Every per-channel loop below is bounded by this.
    const int numCh = juce::jlimit (0, maxChannels, stats.numInputChannels);
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
    for (int ch = 0; ch < numCh; ++ch)
    {
        if (! baselineRecaptureRequested[ch].load (std::memory_order_relaxed))
            continue;

        // A channel in active dropout has a meaningless gain-diff; capturing it
        // would bake a -120 dB "baseline" in. Wait until it recovers, and reset
        // the window whenever capture is interrupted.
        const bool unusable = suppressed
                            || ! stats.inputMonitored[ch]
                            || dropoutState[ch].phase == ConditionState::Phase::active;

        if (unusable)
        {
            baselineAccumSeconds[ch] = 0.0;
            baselineAccumSum[ch]     = 0.0;
            baselineAccumBlocks[ch]  = 0;
            continue;
        }

        baselineAccumSeconds[ch] += blockSeconds;
        baselineAccumSum[ch]     += (double) stats.gainDiffDb[ch];
        ++baselineAccumBlocks[ch];

        if (baselineAccumSeconds[ch] * 1000.0 >= t.baselineCaptureMs && baselineAccumBlocks[ch] > 0)
        {
            baselineGainDiffDb[ch].store ((float) (baselineAccumSum[ch] / (double) baselineAccumBlocks[ch]),
                                          std::memory_order_relaxed);

            baselineRecaptureRequested[ch].store (false, std::memory_order_relaxed);
            baselineAccumSeconds[ch] = 0.0;
            baselineAccumSum[ch]     = 0.0;
            baselineAccumBlocks[ch]  = 0;
        }
    }

    //--------------------------------------------------------------------
    // If detection is suppressed this block, close out any active/pending
    // conditions immediately (end event where applicable) and bail early
    // for dropout/clipping/level-change (but still return current dropout
    // state, which will now be false).
    if (suppressed)
    {
        // Detection pauses, but conditions are deliberately NOT closed out.
        //
        // Closing them here meant every device restart (or any other suppression
        // window) ended the current fault and immediately re-opened it, so a
        // single unresolved problem was re-reported as a new critical error over
        // and over. An ongoing fault must stay the SAME event until it actually
        // clears, so active conditions are carried across the gap and their
        // duration keeps running from the original start.
        //
        // Pending conditions are dropped: they never became events, and letting
        // a half-formed one mature across a suppression gap would date it wrong.
        for (int ch = 0; ch < numCh; ++ch)
        {
            if (dropoutState[ch].phase == ConditionState::Phase::pending)
                dropoutState[ch] = ConditionState{};

            if (clippingState[ch].phase == ConditionState::Phase::pending)
                clippingState[ch] = ConditionState{};

            if (levelChangeState[ch].phase == ConditionState::Phase::pending)
                levelChangeState[ch] = ConditionState{};
        }

        dropoutActiveAtomic.store (false, std::memory_order_relaxed);
        clippingActiveAtomic.store (false, std::memory_order_relaxed);

        return false;
    }

    // Each condition runs independently per input channel, so a fault on one
    // input is neither masked by nor duplicated onto another.
    bool anyDropoutActive = false, anyClippingActive = false;

    for (int ch = 0; ch < numCh; ++ch)
    {
        // An input the user isn't monitoring (nothing patched in) must not report
        // faults: it sits below the dropout threshold forever. Close out anything
        // already in flight so a start event can't be left without its end.
        if (! stats.inputMonitored[ch])
        {
            // Close an in-flight fault properly: silently wiping it would leave a
            // start event in the log with no matching end.
            if (dropoutState[ch].phase == ConditionState::Phase::active)
            {
                Event e;
                e.type           = EventType::CompleteDropout;
                e.severity       = Severity::Critical;
                e.channel        = ChannelId::Both;
                e.channelIndex   = ch;
                e.elapsedSeconds = now;
                e.measuredValue  = now - dropoutState[ch].conditionStartSeconds;
                e.difference     = e.measuredValue;
                e.thresholdUsed  = t.dropoutThresholdDb;
                e.code           = eventCodeEnd;
                tryPush (e);
            }

            if (clippingState[ch].phase == ConditionState::Phase::active)
            {
                Event e;
                e.type           = EventType::Clipping;
                e.severity       = Severity::Warning;
                e.channel        = ChannelId::Both;
                e.channelIndex   = ch;
                e.elapsedSeconds = now;
                e.measuredValue  = now - clippingState[ch].conditionStartSeconds;
                e.difference     = (double) clippingState[ch].accumulatedCount;
                e.thresholdUsed  = t.clipThresholdLinear;
                e.code           = eventCodeEnd;
                tryPush (e);
            }

            if (levelChangeState[ch].phase == ConditionState::Phase::active)
            {
                Event e;
                e.type           = EventType::LevelChange;
                e.severity       = Severity::Warning;
                e.channel        = ChannelId::Both;
                e.channelIndex   = ch;
                e.elapsedSeconds = now;
                e.measuredValue  = now - levelChangeState[ch].conditionStartSeconds;
                e.difference     = levelChangeState[ch].worstValue;
                e.thresholdUsed  = t.levelChangeToleranceDb;
                e.code           = eventCodeEnd;
                tryPush (e);
            }

            dropoutState[ch]     = ConditionState{};
            clippingState[ch]    = ConditionState{};
            levelChangeState[ch] = ConditionState{};
            continue;
        }


        const int refCh = juce::jlimit (0, juce::jmax (0, stats.numOutputChannels - 1),
                                        stats.referenceOutputForInput[ch]);

        //--------------------------------------------------------------------
        // Complete dropout: input RMS (max of L/R) far below the reference.
        {
            const float inRms = stats.inRmsDb[ch];
            const bool conditionNow = inRms <= t.dropoutThresholdDb;

            if (conditionNow)
            {
                if (dropoutState[ch].phase == ConditionState::Phase::inactive)
                {
                    dropoutState[ch].phase = ConditionState::Phase::pending;
                    dropoutState[ch].phaseStartSeconds = now;
                    dropoutState[ch].worstValue = inRms;
                }
                else if (dropoutState[ch].phase == ConditionState::Phase::pending)
                {
                    dropoutState[ch].worstValue = juce::jmin (dropoutState[ch].worstValue, (double) inRms);

                    if ((now - dropoutState[ch].phaseStartSeconds) * 1000.0 >= t.minDropoutMs)
                    {
                        dropoutState[ch].phase = ConditionState::Phase::active;
                        dropoutState[ch].conditionStartSeconds = dropoutState[ch].phaseStartSeconds;

                        Event e;
                        e.type          = EventType::CompleteDropout;
                        e.severity      = Severity::Critical;
                        e.channel       = ChannelId::Both;
                    e.channelIndex   = ch;
                        e.elapsedSeconds = dropoutState[ch].conditionStartSeconds;
                        e.measuredValue = inRms;
                        e.expectedValue = stats.refRmsDb[refCh];
                        e.difference    = (double) inRms - (double) stats.refRmsDb[refCh];
                        e.thresholdUsed = t.dropoutThresholdDb;
                        e.code          = eventCodeStart;
                        tryPush (e);
                    }
                }
                else // active
                {
                    dropoutState[ch].worstValue = juce::jmin (dropoutState[ch].worstValue, (double) inRms);
                }
            }
            else
            {
                if (dropoutState[ch].phase == ConditionState::Phase::active)
                {
                    Event e;
                    e.type          = EventType::CompleteDropout;
                    e.severity      = Severity::Critical;
                    e.channel       = ChannelId::Both;
                    e.channelIndex   = ch;
                    e.elapsedSeconds = now;
                    e.measuredValue = now - dropoutState[ch].conditionStartSeconds;
                    e.expectedValue = 0.0;
                    e.difference    = e.measuredValue;
                    e.thresholdUsed = t.dropoutThresholdDb;
                    e.code          = eventCodeEnd;
                    tryPush (e);
                }
                dropoutState[ch] = ConditionState{};
            }

            anyDropoutActive = anyDropoutActive || dropoutState[ch].phase == ConditionState::Phase::active;
        }

        //--------------------------------------------------------------------
        // Clipping: BlockStats::clipCount > 0 this block. A run ends after
        // clipFreeMs with no clipped samples.
        {
            const bool clippedThisBlock = stats.clipCountPerChannel[ch] > 0;

            if (clippedThisBlock)
            {
                if (clippingState[ch].phase == ConditionState::Phase::inactive)
                {
                    clippingState[ch].phase = ConditionState::Phase::active;
                    clippingState[ch].phaseStartSeconds = now;
                    clippingState[ch].conditionStartSeconds = now;
                    clippingState[ch].accumulatedCount = 0;

                    Event e;
                    e.type          = EventType::Clipping;
                    e.severity      = Severity::Warning;
                    e.channel       = ChannelId::Both;
                    e.channelIndex   = ch;
                    e.elapsedSeconds = now;
                    e.measuredValue = (double) stats.clipCountPerChannel[ch];
                    e.expectedValue = 0.0;
                    e.difference    = (double) stats.clipCountPerChannel[ch];
                    e.thresholdUsed = t.clipThresholdLinear;
                    e.code          = eventCodeStart;
                    tryPush (e);
                }

                clippingState[ch].phaseStartSeconds = now; // last time clipping was observed
                clippingState[ch].accumulatedCount += stats.clipCountPerChannel[ch];
            }
            else if (clippingState[ch].phase == ConditionState::Phase::active)
            {
                if ((now - clippingState[ch].phaseStartSeconds) * 1000.0 >= t.clipFreeMs)
                {
                    const double duration = now - clippingState[ch].conditionStartSeconds;

                    Event e;
                    e.type          = EventType::Clipping;
                    e.severity      = duration * 1000.0 >= t.criticalClipMs ? Severity::Critical : Severity::Warning;
                    e.channel       = ChannelId::Both;
                    e.channelIndex   = ch;
                    e.elapsedSeconds = now;
                    e.measuredValue = duration;
                    e.expectedValue = 0.0;
                    e.difference    = (double) clippingState[ch].accumulatedCount;
                    e.thresholdUsed = t.clipThresholdLinear;
                    e.code          = eventCodeEnd;
                    tryPush (e);

                    clippingState[ch] = ConditionState{};
                }
            }

            anyClippingActive = anyClippingActive || clippingState[ch].phase == ConditionState::Phase::active;
        }

        //--------------------------------------------------------------------
        // Level change: sustained deviation of gainDiffDb from the captured
        // baseline, beyond tolerance.
        {
            const float baseline = baselineGainDiffDb[ch].load (std::memory_order_relaxed);

            if (! std::isnan (baseline))
            {
                const double deviation = (double) stats.gainDiffDb[ch] - (double) baseline;
                const bool conditionNow = std::abs (deviation) > (double) t.levelChangeToleranceDb;

                if (conditionNow)
                {
                    if (levelChangeState[ch].phase == ConditionState::Phase::inactive)
                    {
                        levelChangeState[ch].phase = ConditionState::Phase::pending;
                        levelChangeState[ch].phaseStartSeconds = now;
                        levelChangeState[ch].worstValue = deviation;
                    }
                    else if (levelChangeState[ch].phase == ConditionState::Phase::pending)
                    {
                        if (std::abs (deviation) > std::abs (levelChangeState[ch].worstValue))
                            levelChangeState[ch].worstValue = deviation;

                        if ((now - levelChangeState[ch].phaseStartSeconds) * 1000.0 >= t.levelChangeHoldMs)
                        {
                            levelChangeState[ch].phase = ConditionState::Phase::active;
                            levelChangeState[ch].conditionStartSeconds = levelChangeState[ch].phaseStartSeconds;

                            Event e;
                            e.type          = EventType::LevelChange;
                            e.severity      = Severity::Warning;
                            e.channel       = ChannelId::Both;
                    e.channelIndex   = ch;
                            e.elapsedSeconds = levelChangeState[ch].conditionStartSeconds;
                            e.measuredValue = (double) stats.gainDiffDb[ch];
                            e.expectedValue = (double) baseline;
                            e.difference    = deviation;
                            e.thresholdUsed = t.levelChangeToleranceDb;
                            e.code          = eventCodeStart;
                            tryPush (e);
                        }
                    }
                    else // active
                    {
                        if (std::abs (deviation) > std::abs (levelChangeState[ch].worstValue))
                            levelChangeState[ch].worstValue = deviation;
                    }
                }
                else
                {
                    if (levelChangeState[ch].phase == ConditionState::Phase::active)
                    {
                        Event e;
                        e.type          = EventType::LevelChange;
                        e.severity      = Severity::Warning;
                        e.channel       = ChannelId::Both;
                    e.channelIndex   = ch;
                        e.elapsedSeconds = now;
                        e.measuredValue = now - levelChangeState[ch].conditionStartSeconds;
                        e.expectedValue = 0.0;
                        e.difference    = levelChangeState[ch].worstValue;
                        e.thresholdUsed = t.levelChangeToleranceDb;
                        e.code          = eventCodeEnd;
                        tryPush (e);

                        // Re-arm the baseline to the new settled level.
                        recaptureBaseline (ch);
                    }
                    levelChangeState[ch] = ConditionState{};
                }
            }
        }
    }

    dropoutActiveAtomic.store (anyDropoutActive, std::memory_order_relaxed);
    clippingActiveAtomic.store (anyClippingActive, std::memory_order_relaxed);

    return dropoutActiveAtomic.load (std::memory_order_relaxed);
}

} // namespace pnam
