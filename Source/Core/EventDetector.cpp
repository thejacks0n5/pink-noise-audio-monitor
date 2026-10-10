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

    /** A dropout ends only once the input is this far above the level it had to
        fall to in order to start. Without the gap an input sitting right at
        the entry level starts and ends a "new" dropout every few seconds. This
        is the MAXIMUM gap: see dropoutExitLevel(). */
    constexpr double dropoutExitHysteresisDb = 6.0;

    /** Exit level for a dropout entered at entryLevelDb when the input is
        normally at normalLevelDb. The hysteresis may not exceed half the
        distance between the two, or on a quiet loop (normal return within a
        few dB of the absolute floor) the exit level sits above anything the
        input ever reaches: the dropout never ends, and one stray block starts
        a "dropout" that is never cancelled. */
    inline double dropoutExitLevel (double entryLevelDb, double normalLevelDb) noexcept
    {
        return entryLevelDb + juce::jlimit (0.0, dropoutExitHysteresisDb, 0.5 * (normalLevelDb - entryLevelDb));
    }

    /** Before a baseline exists the input's usual level can turn out to be
        within a few dB of the entry level (a lossy loop returning at -58 dBFS
        against a -60 dBFS floor). minDropoutMs of pink noise cannot tell that
        from a dead input 1 dB under the floor, so in that squeezed case a
        pending dropout is confirmed early only if it is this far below the
        entry level, and otherwise on the 400 ms smoothed level once it has been
        pending this long. The Start is back-dated to when it began either way. */
    constexpr double ambiguousDropoutMarginDb = 3.0;
    constexpr double ambiguousDropoutHoldMs   = 1000.0;

    /** One non-finite sample (a driver glitch) makes a whole block's figures
        NaN/Inf; left alone that poisons every running sum it is added to. */
    inline double finiteOr (double value, double fallback) noexcept
    {
        return std::isfinite (value) ? value : fallback;
    }

    /** Time constant of the energy smoothers behind the level comparison. Long
        enough to average out the round-trip lag between the reference block
        and the returned block, short against levelChangeHoldMs. */
    constexpr double levelSmoothingSeconds = 0.4;

    /** Mean-square -> dBFS with the same floor clamp as the per-block dB values. */
    inline double meanSquareToDb (double meanSquare) noexcept
    {
        if (meanSquare <= 0.0)
            return (double) silenceFloorDb;

        const double db = 10.0 * std::log10 (meanSquare);
        return db < (double) silenceFloorDb ? (double) silenceFloorDb : db;
    }
}

//==============================================================================
EventDetector::EventDetector (EventQueue& queueToFill)
    : eventQueue (queueToFill)
{
    thresholdShared   = Thresholds{};
    currentThresholds = Thresholds{};
    settlingMsAtomic.store (thresholdShared.settlingMs, std::memory_order_relaxed);

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

        // An exit hold has to be continuous; it restarts on the new device.
        dropoutState[ch].clearing     = false;
        levelChangeState[ch].clearing = false;

        // The round-trip gain is device-dependent, so it must be recaptured —
        // unless this channel has a level change ACTIVE. Its baseline is the
        // only record of what "normal" was: recapturing now would measure the
        // faulty level, adopt it, and end the fault a few seconds after every
        // restart with the change still there. It is recaptured when the level
        // change genuinely ends, as always.
        if (levelChangeState[ch].phase != ConditionState::Phase::active)
        {
            baselineGainDiffDb[ch].store (std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
            baselineRecaptureRequested[ch].store (true, std::memory_order_relaxed);
        }

        baselineAccumSeconds[ch] = 0.0;
        baselineAccumSum[ch]     = 0.0;
        baselineAccumBlocks[ch]  = 0;

        // Levels on the new device are unrelated to the old ones.
        smoothedRefMeanSquare[ch] = 0.0;
        smoothedInMeanSquare[ch]  = 0.0;
    }

    smoothedBlockCount = 0;

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

        smoothedRefMeanSquare[ch] = 0.0;
        smoothedInMeanSquare[ch]  = 0.0;
    }

    smoothedBlockCount     = 0;
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

    settlingMsAtomic.store (newThresholds.settlingMs, std::memory_order_relaxed);
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

    // Every per-channel loop below is bounded by these.
    const int numCh  = juce::jlimit (0, maxChannels, stats.numInputChannels);
    const int numOut = juce::jlimit (0, maxChannels, stats.numOutputChannels);
    const double blockSeconds = sampleRate > 0.0 ? (double) stats.numSamples / sampleRate : 0.0;
    const bool suppressed = isSuppressed (stats.statusCode);

    //--------------------------------------------------------------------
    // Rate limiter bookkeeping (rolling one-second window).
    if (now - rateWindowStartSeconds >= 1.0)
    {
        rateWindowStartSeconds = now;
        eventsThisWindow = 0;
    }

    // Start events are rate-limited; a refused one is NOT lost — see reportStart.
    // The free-space check keeps a push we are able to retry out of the queue's
    // dropped-record counter.
    auto tryPushStart = [this, &t] (const Event& e) noexcept -> bool
    {
        if (eventsThisWindow >= t.maxEventsPerSecond || eventQueue.getFreeSpace() < 1)
            return false;

        if (eventQueue.push (e))
        {
            ++eventsThisWindow;
            return true;
        }
        return false;
    };

    // End events are never rate-limited: an End held back would leave its Start
    // looking like an unresolved fault. Only a full queue can refuse one.
    auto tryPushEnd = [this] (const Event& e) noexcept -> bool
    {
        return eventQueue.getFreeSpace() > 0 && eventQueue.push (e);
    };

    // A confirmed condition whose Start has not reached the queue yet keeps
    // offering it, block after block, until it is accepted.
    auto reportStart = [&tryPushStart] (ConditionState& s) noexcept
    {
        if (s.phase == ConditionState::Phase::active && ! s.startReported)
            s.startReported = tryPushStart (s.startEvent);
    };

    // Closes an active condition: Start first if it is still owed, then the End.
    // The state is only reset once BOTH are in the queue; until then it stays
    // active and the caller simply tries again next block.
    auto finish = [&reportStart, &tryPushEnd] (ConditionState& s, const Event& endEvent) noexcept -> bool
    {
        reportStart (s);

        if (! s.startReported || ! tryPushEnd (endEvent))
            return false;

        s = ConditionState{};
        return true;
    };

    // Owed Starts are retried whether or not detection is suppressed this
    // block, and for every slot (a channel that has since disappeared must
    // still get its Start out).
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        reportStart (dropoutState[ch]);
        reportStart (clippingState[ch]);
        reportStart (levelChangeState[ch]);
    }

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

        // A non-finite figure must not reach the sum: the captured baseline
        // would be NaN, i.e. "no baseline", with nothing left to re-request it.
        if (! std::isfinite (stats.gainDiffDb[ch]))
            continue;

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
        // A half-timed exit is dropped for the same reason: the fault has to be
        // seen clear, continuously, with detection running.
        for (int ch = 0; ch < numCh; ++ch)
        {
            if (dropoutState[ch].phase == ConditionState::Phase::pending)
                dropoutState[ch] = ConditionState{};

            if (clippingState[ch].phase == ConditionState::Phase::pending)
                clippingState[ch] = ConditionState{};

            if (levelChangeState[ch].phase == ConditionState::Phase::pending)
                levelChangeState[ch] = ConditionState{};

            dropoutState[ch].clearing     = false;
            levelChangeState[ch].clearing = false;
        }

        // The reference is muted, fading or absent: whatever the smoothers held
        // no longer describes the loop. They start again from scratch.
        smoothedBlockCount = 0;

        dropoutActiveAtomic.store (false, std::memory_order_relaxed);
        clippingActiveAtomic.store (false, std::memory_order_relaxed);

        return false;
    }

    //--------------------------------------------------------------------
    // Energy smoothing for the level comparison. One coefficient per block
    // (blocks are not all the same length), applied to every channel. For the
    // first ~time-constant after a restart the weight is 1/n, i.e. a plain
    // running mean, so the estimate starts converged.
    {
        ++smoothedBlockCount;

        const double pole  = blockSeconds > 0.0 ? 1.0 - std::exp (-blockSeconds / levelSmoothingSeconds) : 1.0;
        const double alpha = juce::jmax (pole, 1.0 / (double) smoothedBlockCount);

        // Non-finite block energies count as silence. The first block after a
        // restart is assigned, not blended, so nothing the smoother held before
        // (whatever it was) can survive into the new estimate.
        const bool first = smoothedBlockCount == 1;

        for (int ch = 0; ch < numOut; ++ch)
        {
            const double v = finiteOr (stats.refMeanSquare[ch], 0.0);
            smoothedRefMeanSquare[ch] = first ? v : smoothedRefMeanSquare[ch] + alpha * (v - smoothedRefMeanSquare[ch]);
        }

        for (int ch = 0; ch < numCh; ++ch)
        {
            const double v = finiteOr (stats.inMeanSquare[ch], 0.0);
            smoothedInMeanSquare[ch] = first ? v : smoothedInMeanSquare[ch] + alpha * (v - smoothedInMeanSquare[ch]);
        }
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
            // start event in the log with no matching end. If the End cannot be
            // pushed this block the state is left active and closed on a later one.
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
                e.thresholdUsed  = dropoutState[ch].entryLevelDb;
                e.code           = eventCodeEnd;
                finish (dropoutState[ch], e);
            }
            else
            {
                dropoutState[ch] = ConditionState{};
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
                finish (clippingState[ch], e);
            }
            else
            {
                clippingState[ch] = ConditionState{};
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
                finish (levelChangeState[ch], e);
            }
            else
            {
                levelChangeState[ch] = ConditionState{};
            }

            anyDropoutActive  = anyDropoutActive  || dropoutState[ch].phase  == ConditionState::Phase::active;
            anyClippingActive = anyClippingActive || clippingState[ch].phase == ConditionState::Phase::active;
            continue;
        }


        const int refCh = juce::jlimit (0, juce::jmax (0, stats.numOutputChannels - 1),
                                        stats.referenceOutputForInput[ch]);

        const float baseline = baselineGainDiffDb[ch].load (std::memory_order_relaxed);
        const bool  haveBaseline = ! std::isnan (baseline);
        const double smoothedRefDb = meanSquareToDb (smoothedRefMeanSquare[refCh]);

        // This block's own figures, with a non-finite block read as silence.
        const float blockRefDb = (float) finiteOr ((double) stats.refRmsDb[refCh], (double) silenceFloorDb);

        //--------------------------------------------------------------------
        // Complete dropout: this block's input RMS at/below the entry level —
        // dropoutRelativeDb below what the reference and the captured loop gain
        // say the input should be (see below for the time before a baseline).
        // The input side is deliberately NOT smoothed: a 400 ms smoother takes
        // about 2 s to fall 20 dB and would hide every short dropout. The
        // prediction is smoothed, and the hysteresis + hold below are what stop
        // a marginal level from flapping.
        {
            auto& d = dropoutState[ch];
            const float inRms = (float) finiteOr ((double) stats.inRmsDb[ch], (double) silenceFloorDb);
            const double inMeanSquare = finiteOr (stats.inMeanSquare[ch], 0.0);
            const double floorDb = (double) t.dropoutThresholdDb;

            // The reference level to judge against right now: never more than
            // this block's own reference says. The smoothed reference takes ~2 s
            // to follow a 20 dB cut, the input follows it within the round-trip
            // latency, and judging one against the other reported every
            // deliberate output-level reduction as a dropout. Taking the lower of
            // the two lets the yardstick fall at once and still rise slowly.
            const double refNowDb = juce::jmin (smoothedRefDb, (double) blockRefDb);

            double entryLevel = floorDb, exitLevel = floorDb;

            if (haveBaseline)
            {
                // RELATIVE ONLY. Once there is a prediction the absolute floor has
                // no say: with the output turned down, a perfectly healthy return
                // sits a few dB above -60 dBFS (or below it) and ordinary
                // block-to-block variation of pink noise crosses any fixed floor
                // — a UMC404HD loop at -40 dBFS output logged 15 dropout events
                // in 8 seconds. An input 6 dB under its prediction is not a
                // dropout at any absolute level; one 20 dB under it is.
                const double predictedDb = refNowDb + (double) baseline;

                entryLevel = predictedDb - (double) t.dropoutRelativeDb;
                exitLevel  = dropoutExitLevel (entryLevel, predictedDb);

                // The levels a dropout in flight keeps if the baseline goes away
                // (device restart) come from the SLOW estimate, not from whatever
                // this one block's reference happened to be.
                if (d.phase != ConditionState::Phase::inactive)
                {
                    const double normalDb = smoothedRefDb + (double) baseline;
                    d.entryLevelDb = normalDb - (double) t.dropoutRelativeDb;
                    d.exitLevelDb  = dropoutExitLevel (d.entryLevelDb, normalDb);
                }
            }
            else if (d.phase != ConditionState::Phase::inactive)
            {
                // No baseline (not captured yet, or discarded by a device restart)
                // but a dropout is in flight: it keeps the levels it had.
                entryLevel = d.entryLevelDb;
                exitLevel  = d.exitLevelDb;
            }
            else
            {
                // No baseline yet and nothing in flight (the first seconds after
                // a start). The loop gain is unknown, so the reference itself is
                // the yardstick: a dead input is one below the absolute floor AND
                // dropoutRelativeDb below the reference. The second condition is
                // what stops a healthy return at a low output level being called
                // dead; the first keeps a lossy-but-alive loop at normal level
                // out of it. The only evidence of this input's normal level is
                // where its smoothed level sits now, which sets the exit level.
                entryLevel = juce::jmin (floorDb, refNowDb - (double) t.dropoutRelativeDb);
                exitLevel  = dropoutExitLevel (entryLevel, meanSquareToDb (smoothedInMeanSquare[ch]));
            }

            const bool below = (double) inRms <= entryLevel;

            // "Back above the exit level": normally this block is. When the
            // hysteresis had to be squeezed (the input's usual level is less than
            // 12 dB above the entry level — only possible before a baseline
            // exists) single blocks straddle both levels all the time, so there
            // the question is put to the input averaged over the last
            // minDropoutMs or so instead. The average exists only while a dropout
            // is in flight and starts from the block that began it, so it carries
            // no memory of the level before the dropout.
            bool recovered = (double) inRms > exitLevel;
            const bool squeezed = exitLevel - entryLevel < dropoutExitHysteresisDb - 0.01;
            double recentDb = (double) inRms;

            if (d.phase != ConditionState::Phase::inactive)
            {
                const double window = juce::jmax (0.001, t.minDropoutMs / 1000.0);
                const double weight = blockSeconds > 0.0 ? 1.0 - std::exp (-blockSeconds / window) : 1.0;

                d.recentMeanSquare += weight * (inMeanSquare - d.recentMeanSquare);
                recentDb = meanSquareToDb (d.recentMeanSquare);

                if (squeezed)
                    recovered = recentDb > exitLevel;
            }

            if (d.phase == ConditionState::Phase::inactive)
            {
                if (below)
                {
                    d.phase = ConditionState::Phase::pending;
                    d.phaseStartSeconds = now;
                    d.worstValue = inRms;
                    d.recentMeanSquare = inMeanSquare;

                    if (! haveBaseline)
                    {
                        d.entryLevelDb = entryLevel;
                        d.exitLevelDb  = exitLevel;
                    }
                    else
                    {
                        const double normalDb = smoothedRefDb + (double) baseline;
                        d.entryLevelDb = normalDb - (double) t.dropoutRelativeDb;
                        d.exitLevelDb  = dropoutExitLevel (d.entryLevelDb, normalDb);
                    }
                }
            }
            else if (d.phase == ConditionState::Phase::pending)
            {
                // Same hysteresis on the way in as on the way out: once the input
                // has touched the entry level, only climbing clear of it cancels
                // the pending dropout. Requiring EVERY block to be below the entry
                // level meant a floor sitting a fraction of a dB under it (short
                // blocks scatter by more than that) took seconds to be confirmed.
                if (recovered)
                {
                    d = ConditionState{};
                }
                else
                {
                    d.worstValue = juce::jmin (d.worstValue, (double) inRms);

                    const double pendingMs = (now - d.phaseStartSeconds) * 1000.0;
                    bool confirmed = pendingMs >= t.minDropoutMs;

                    if (confirmed && squeezed)
                    {
                        // Too close to call on minDropoutMs of signal (see
                        // ambiguousDropoutMarginDb): confirm now only if it is
                        // clearly below, otherwise let the slow average decide.
                        const bool longEnough = pendingMs >= juce::jmax (t.minDropoutMs, ambiguousDropoutHoldMs);

                        confirmed = recentDb <= entryLevel - ambiguousDropoutMarginDb
                                      || (longEnough && meanSquareToDb (smoothedInMeanSquare[ch]) <= entryLevel);

                        if (longEnough && ! confirmed)
                            d = ConditionState{};   // watched for long enough: on average it is above the entry level
                    }

                    if (confirmed)
                    {
                        d.phase = ConditionState::Phase::active;
                        d.conditionStartSeconds = d.phaseStartSeconds;

                        Event e;
                        e.type          = EventType::CompleteDropout;
                        e.severity      = Severity::Critical;
                        e.channel       = ChannelId::Both;
                        e.channelIndex   = ch;
                        e.elapsedSeconds = d.conditionStartSeconds;
                        e.measuredValue = inRms;
                        e.expectedValue = blockRefDb;
                        e.difference    = (double) inRms - (double) blockRefDb;
                        e.thresholdUsed = d.entryLevelDb;
                        e.code          = eventCodeStart;

                        d.startEvent = e;
                        reportStart (d);
                    }
                }
            }
            else // active
            {
                if (recovered)
                {
                    if (! d.clearing)
                    {
                        d.clearing = true;
                        d.clearStartSeconds = now;
                    }

                    if ((now - d.clearStartSeconds) * 1000.0 >= t.minDropoutMs)
                    {
                        // Stamped at the moment the input came back, not at the
                        // end of the hold that confirmed it.
                        Event e;
                        e.type          = EventType::CompleteDropout;
                        e.severity      = Severity::Critical;
                        e.channel       = ChannelId::Both;
                        e.channelIndex   = ch;
                        e.elapsedSeconds = d.clearStartSeconds;
                        e.measuredValue = d.clearStartSeconds - d.conditionStartSeconds;
                        e.expectedValue = 0.0;
                        e.difference    = e.measuredValue;
                        e.thresholdUsed = d.entryLevelDb;
                        e.code          = eventCodeEnd;
                        finish (d, e);
                    }
                }
                else
                {
                    d.clearing = false;
                    d.worstValue = juce::jmin (d.worstValue, (double) inRms);
                }
            }

            anyDropoutActive = anyDropoutActive || d.phase == ConditionState::Phase::active;
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

                    clippingState[ch].startEvent = e;
                    reportStart (clippingState[ch]);
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
                    finish (clippingState[ch], e);
                }
            }

            anyClippingActive = anyClippingActive || clippingState[ch].phase == ConditionState::Phase::active;
        }

        //--------------------------------------------------------------------
        // Level change: sustained deviation of the SMOOTHED round-trip gain from
        // the captured baseline, beyond tolerance.
        {
            auto& lc = levelChangeState[ch];

            if (dropoutState[ch].phase == ConditionState::Phase::active)
            {
                // A dropout is by definition also a huge level change. It is one
                // fault and is reported once, as the dropout: a level change that
                // was forming is discarded, and one that was already active is
                // frozen until the dropout is over.
                if (lc.phase == ConditionState::Phase::pending)
                    lc = ConditionState{};

                lc.clearing = false;
            }
            else if (dropoutState[ch].phase == ConditionState::Phase::pending)
            {
                // Might be the first milliseconds of a dropout: this block is not
                // judged as a level change either way. Deliberately NOT a reset —
                // a level sitting near the dropout entry point trips "pending"
                // on stray blocks, and resetting on each of them would stop a
                // genuine level change there from ever being confirmed.
            }
            else if (haveBaseline)
            {
                const double smoothedGainDb = meanSquareToDb (smoothedInMeanSquare[ch]) - smoothedRefDb;
                const double deviation = smoothedGainDb - (double) baseline;
                const bool conditionNow = std::abs (deviation) > (double) t.levelChangeToleranceDb;

                if (lc.phase == ConditionState::Phase::inactive)
                {
                    if (conditionNow)
                    {
                        lc.phase = ConditionState::Phase::pending;
                        lc.phaseStartSeconds = now;
                        lc.worstValue = deviation;
                    }
                }
                else if (lc.phase == ConditionState::Phase::pending)
                {
                    if (! conditionNow)
                    {
                        lc = ConditionState{};
                    }
                    else
                    {
                        if (std::abs (deviation) > std::abs (lc.worstValue))
                            lc.worstValue = deviation;

                        if ((now - lc.phaseStartSeconds) * 1000.0 >= t.levelChangeHoldMs)
                        {
                            lc.phase = ConditionState::Phase::active;
                            lc.conditionStartSeconds = lc.phaseStartSeconds;

                            Event e;
                            e.type          = EventType::LevelChange;
                            e.severity      = Severity::Warning;
                            e.channel       = ChannelId::Both;
                            e.channelIndex   = ch;
                            e.elapsedSeconds = lc.conditionStartSeconds;
                            e.measuredValue = smoothedGainDb;
                            e.expectedValue = (double) baseline;
                            e.difference    = deviation;
                            e.thresholdUsed = t.levelChangeToleranceDb;
                            e.code          = eventCodeStart;

                            lc.startEvent = e;
                            reportStart (lc);
                        }
                    }
                }
                else // active
                {
                    if (conditionNow)
                    {
                        lc.clearing = false;

                        if (std::abs (deviation) > std::abs (lc.worstValue))
                            lc.worstValue = deviation;
                    }
                    else
                    {
                        // Back within tolerance — but one in-tolerance reading is
                        // not "over". It has to hold, exactly as the start did.
                        if (! lc.clearing)
                        {
                            lc.clearing = true;
                            lc.clearStartSeconds = now;
                        }

                        if ((now - lc.clearStartSeconds) * 1000.0 >= t.levelChangeHoldMs)
                        {
                            Event e;
                            e.type          = EventType::LevelChange;
                            e.severity      = Severity::Warning;
                            e.channel       = ChannelId::Both;
                            e.channelIndex   = ch;
                            e.elapsedSeconds = lc.clearStartSeconds;
                            e.measuredValue = lc.clearStartSeconds - lc.conditionStartSeconds;
                            e.expectedValue = 0.0;
                            e.difference    = lc.worstValue;
                            e.thresholdUsed = t.levelChangeToleranceDb;
                            e.code          = eventCodeEnd;

                            // Re-arm the baseline to the new settled level.
                            if (finish (lc, e))
                                recaptureBaseline (ch);
                        }
                    }
                }
            }
        }
    }

    dropoutActiveAtomic.store (anyDropoutActive, std::memory_order_relaxed);
    clippingActiveAtomic.store (anyClippingActive, std::memory_order_relaxed);

    return dropoutActiveAtomic.load (std::memory_order_relaxed);
}

} // namespace pnam
