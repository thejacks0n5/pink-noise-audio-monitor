/*
  ==============================================================================
    AnalysisProcessor.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "AnalysisProcessor.h"
#include "ArtifactDetector.h"
#include <cmath>

namespace pnam
{

namespace
{
    /** Largest block this processor will analyse in one call. Reference
        samples are copied into a preallocated scratch buffer of this length so
        the audio thread never allocates; blocks larger than this are
        truncated for analysis purposes (JUCE devices practically never
        exceed a few thousand samples per callback). */
    constexpr int maxAnalysedBlockSamples = 8192;

    /** Converts a linear amplitude to dBFS, clamped to silenceFloorDb. */
    inline float linearToDb (float linear) noexcept
    {
        if (linear <= 0.0f)
            return silenceFloorDb;

        const float db = 20.0f * std::log10 (linear);
        return db < silenceFloorDb ? silenceFloorDb : db;
    }

    /** Peak (max abs) and sum-of-squares of one channel block. */
    inline void analyseChannel (const float* data, int numSamples, float& peakOut, double& sumSquaresOut) noexcept
    {
        float peak = 0.0f;
        double sumSquares = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const float sample = data[i];
            const float absSample = std::abs (sample);
            if (absSample > peak)
                peak = absSample;

            sumSquares += (double) sample * (double) sample;
        }

        peakOut = peak;
        sumSquaresOut = sumSquares;
    }
}

//==============================================================================
AnalysisProcessor::AnalysisProcessor (MeasurementQueue& queueToFill)
    : measurementQueue (queueToFill)
{
    // EVERY channel must start at the silence floor. These atomics hold dB
    // values directly, so a default-constructed (zero) one reads as 0 dBFS —
    // which the meters draw as a full-scale red bar. That is what appeared on
    // launch, before prepare() had run, for the reference meters and for any
    // input channel past the first two.
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        atomicRefPeakDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicRefRmsDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicInPeakDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicInRmsDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicRefPeakHoldDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicInPeakHoldDb[ch].store (silenceFloorDb, std::memory_order_relaxed);

        // Same reasoning: these are read before prepare() too. Default-off
        // monitoring would silently disable detection on every input.
        referenceOutputForInput[ch].store (0, std::memory_order_relaxed);
        inputMonitoredFlags[ch].store (true, std::memory_order_relaxed);
    }
}

//==============================================================================
void AnalysisProcessor::prepare (double newSampleRate, int maximumBlockSize, int newNumInputChannels,
                                 int newNumOutputChannels)
{
    // Fold whatever the previous device configuration accumulated into the
    // session offset BEFORE adopting the new sample rate, so the clock keeps
    // moving forward across a device restart instead of rewinding to zero.
    if (sampleRate > 0.0 && samplesProcessed > 0)
        elapsedOffsetSeconds += (double) samplesProcessed / sampleRate;

    sampleRate = newSampleRate;
    maxBlock   = maximumBlockSize;
    numInCh    = juce::jlimit (0, maxChannels, newNumInputChannels);
    numOutCh   = juce::jlimit (0, maxChannels, newNumOutputChannels);

    samplesProcessed    = 0;
    samplesIntoInterval = 0;

    // Allocated here (non-real-time, callback idle) and never resized again, so
    // the audio thread only ever reads/writes existing storage.
    if ((int) refScratch.size() != maxAnalysedBlockSamples)
        refScratch.assign ((size_t) maxAnalysedBlockSamples, 0.0f);

    for (int ch = 0; ch < maxChannels; ++ch)
    {
        refSumSquares[ch]   = 0.0;
        inSumSquares[ch]    = 0.0;
        refIntervalPeak[ch] = 0.0f;
        inIntervalPeak[ch]  = 0.0f;
        intervalClipCountPerChannel[ch] = 0;

        atomicRefPeakDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicRefRmsDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicInPeakDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicInRmsDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicRefPeakHoldDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicInPeakHoldDb[ch].store (silenceFloorDb, std::memory_order_relaxed);

        // Default pairing: input n references output n, clamped to what exists.
        referenceOutputForInput[ch].store (juce::jlimit (0, juce::jmax (0, numOutCh - 1), ch),
                                           std::memory_order_relaxed);

        // Monitored unless the user turns it off; MainComponent re-applies the
        // saved per-channel choice right after the device is prepared.
        inputMonitoredFlags[ch].store (true, std::memory_order_relaxed);
    }

    intervalClipCount    = 0;
    intervalStatusBits   = StatusBits::ok;
    intervalDropoutSeen  = false;

    atomicElapsedSeconds.store (elapsedOffsetSeconds, std::memory_order_relaxed);
}

void AnalysisProcessor::resetSessionClock() noexcept
{
    elapsedOffsetSeconds = 0.0;
    samplesProcessed     = 0;
    samplesIntoInterval  = 0;
    atomicElapsedSeconds.store (0.0, std::memory_order_relaxed);
}

//==============================================================================
void AnalysisProcessor::setMeasurementIntervalSeconds (double seconds) noexcept
{
    measurementIntervalSeconds.store ((float) juce::jlimit (0.1, 60.0, seconds), std::memory_order_relaxed);
}

double AnalysisProcessor::getMeasurementIntervalSeconds() const noexcept
{
    return (double) measurementIntervalSeconds.load (std::memory_order_relaxed);
}

void AnalysisProcessor::setClipThresholdLinear (float threshold) noexcept
{
    clipThresholdLinear.store (threshold, std::memory_order_relaxed);
}

//==============================================================================
BlockStats AnalysisProcessor::processBlock (const float* const* input, int numInputChannelsThisBlock, int numSamples,
                                             const ReferenceHistory& reference,
                                             int32_t statusBitsFromEngine) noexcept
{
    BlockStats stats;
    stats.blockStartElapsedSeconds = getElapsedSeconds();
    stats.numSamples       = numSamples;
    stats.numInputChannels = numInputChannelsThisBlock;
    stats.statusCode       = statusBitsFromEngine;

    // Bounded by the scratch buffer prepare() allocated (empty before the first
    // prepare(), in which case nothing is analysed rather than anything read
    // out of bounds).
    const int samplesToUse = juce::jlimit (0, juce::jmin (maxAnalysedBlockSamples, (int) refScratch.size()),
                                           juce::jmin (numSamples, maxBlock > 0 ? maxBlock : numSamples));

    // One fixed-size scratch buffer, reused per channel — a per-channel array
    // would be 64x larger for no benefit. A member rather than stack storage
    // keeps the audio-thread stack frame small.
    float* const refScratchData = refScratch.data();

    //--------------------------------------------------------------------
    // Reference: one independent stream per output channel.
    const int refChannels = juce::jlimit (1, maxChannels,
                                          juce::jmax (1, reference.getNumChannels()));
    stats.numOutputChannels = refChannels;

    float  refPeakLinearBlock[maxChannels] {};
    double refSumSquaresBlock[maxChannels] {};

    for (int ch = 0; ch < refChannels; ++ch)
    {
        reference.readLatest (refScratchData, ch, samplesToUse, 0);

        float peak = 0.0f;
        double sumSquares = 0.0;
        analyseChannel (refScratchData, samplesToUse, peak, sumSquares);

        refPeakLinearBlock[ch] = peak;
        refSumSquaresBlock[ch] = sumSquares;
        stats.refMeanSquare[ch] = sumSquares / (double) juce::jmax (1, samplesToUse);

        const float peakDb = linearToDb (peak);
        const float rmsDb  = linearToDb ((float) std::sqrt (sumSquares / (double) juce::jmax (1, samplesToUse)));

        stats.refPeakDb[ch] = peakDb;
        stats.refRmsDb[ch]  = rmsDb;

        atomicRefPeakDb[ch].store (peakDb, std::memory_order_relaxed);
        atomicRefRmsDb[ch].store (rmsDb, std::memory_order_relaxed);

        if (peakDb > atomicRefPeakHoldDb[ch].load (std::memory_order_relaxed))
            atomicRefPeakHoldDb[ch].store (peakDb, std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------
    // Input channels.
    const float clipThreshold = clipThresholdLinear.load (std::memory_order_relaxed);
    int32_t blockClipCount = 0;

    float  inPeakLinear[maxChannels] {};
    double inSumSquaresBlock[maxChannels] {};

    const int channelsToAnalyse = juce::jlimit (0, maxChannels, numInputChannelsThisBlock);

    for (int ch = 0; ch < channelsToAnalyse; ++ch)
    {
        const float* data = input != nullptr ? input[ch] : nullptr;
        if (data == nullptr)
            continue;

        float peak = 0.0f;
        double sumSquares = 0.0;
        analyseChannel (data, samplesToUse, peak, sumSquares);

        inPeakLinear[ch] = peak;
        inSumSquaresBlock[ch] = sumSquares;

        int32_t channelClips = 0;
        for (int i = 0; i < samplesToUse; ++i)
            if (std::abs (data[i]) >= clipThreshold)
                ++channelClips;

        stats.clipCountPerChannel[ch] = channelClips;
        blockClipCount += channelClips;
    }

    for (int ch = 0; ch < channelsToAnalyse; ++ch)
    {
        const float peakDb = linearToDb (inPeakLinear[ch]);
        const float rmsDb  = linearToDb ((float) std::sqrt (inSumSquaresBlock[ch]
                                          / (double) juce::jmax (1, samplesToUse)));

        stats.inPeakDb[ch] = peakDb;
        stats.inRmsDb[ch]  = rmsDb;
        stats.inMeanSquare[ch] = inSumSquaresBlock[ch] / (double) juce::jmax (1, samplesToUse);

        // Compare against whichever output this input is referenced to.
        const int refCh = juce::jlimit (0, refChannels - 1,
                                        referenceOutputForInput[ch].load (std::memory_order_relaxed));
        stats.gainDiffDb[ch] = rmsDb - stats.refRmsDb[refCh];
        stats.referenceOutputForInput[ch] = refCh;
        stats.inputMonitored[ch] = inputMonitoredFlags[ch].load (std::memory_order_relaxed);

        atomicInPeakDb[ch].store (peakDb, std::memory_order_relaxed);
        atomicInRmsDb[ch].store (rmsDb, std::memory_order_relaxed);

        if (peakDb > atomicInPeakHoldDb[ch].load (std::memory_order_relaxed))
            atomicInPeakHoldDb[ch].store (peakDb, std::memory_order_relaxed);
    }

    stats.clipCount = blockClipCount;

    //--------------------------------------------------------------------
    // Interval accumulation for the next Measurement push.
    for (int ch = 0; ch < refChannels; ++ch)
    {
        refSumSquares[ch]   += refSumSquaresBlock[ch];
        refIntervalPeak[ch]  = juce::jmax (refIntervalPeak[ch], refPeakLinearBlock[ch]);
    }

    for (int ch = 0; ch < channelsToAnalyse; ++ch)
    {
        inSumSquares[ch]   += inSumSquaresBlock[ch];
        inIntervalPeak[ch]  = juce::jmax (inIntervalPeak[ch], inPeakLinear[ch]);
        intervalClipCountPerChannel[ch] += stats.clipCountPerChannel[ch];
    }

    intervalClipCount        += blockClipCount;
    intervalStatusBits       |= statusBitsFromEngine;

    samplesProcessed    += samplesToUse;
    samplesIntoInterval += samplesToUse;

    if (sampleRate > 0.0)
        atomicElapsedSeconds.store (elapsedOffsetSeconds + (double) samplesProcessed / sampleRate,
                                    std::memory_order_relaxed);

    //--------------------------------------------------------------------
    // Push a Measurement at each interval boundary.
    const double intervalSeconds = (double) measurementIntervalSeconds.load (std::memory_order_relaxed);
    const int64_t intervalSampleThreshold = sampleRate > 0.0
        ? (int64_t) (intervalSeconds * sampleRate)
        : (int64_t) 0;

    if (intervalSampleThreshold > 0 && samplesIntoInterval >= intervalSampleThreshold)
    {
        const int64_t samplesForAverage = juce::jmax ((int64_t) 1, samplesIntoInterval);

        Measurement m;
        m.elapsedSeconds = getElapsedSeconds();
        m.sampleRate     = sampleRate;
        // The device buffer size, not this callback's block length — CoreAudio
        // may deliver varying block sizes within one configuration.
        m.bufferSize     = maxBlock;

        m.numOutputChannels = refChannels;
        m.numInputChannels  = channelsToAnalyse;

        for (int ch = 0; ch < refChannels; ++ch)
        {
            m.refPeakDb[ch] = linearToDb (refIntervalPeak[ch]);
            m.refRmsDb[ch]  = linearToDb ((float) std::sqrt (refSumSquares[ch] / (double) samplesForAverage));
        }

        for (int ch = 0; ch < channelsToAnalyse; ++ch)
        {
            m.inPeakDb[ch] = linearToDb (inIntervalPeak[ch]);
            m.inRmsDb[ch]  = linearToDb ((float) std::sqrt (inSumSquares[ch] / (double) samplesForAverage));

            const int refCh = juce::jlimit (0, refChannels - 1,
                                            referenceOutputForInput[ch].load (std::memory_order_relaxed));
            m.gainDiffDb[ch] = m.inRmsDb[ch] - m.refRmsDb[refCh];
            m.clipCountPerChannel[ch] = intervalClipCountPerChannel[ch];

            if (artifactDetector != nullptr)
            {
                const auto figures = artifactDetector->takeIntervalFigures (ch);
                m.residualDb[ch]      = figures.residualDb;
                m.latencyMs[ch]       = figures.latencyMs;
                m.clickCount[ch]      = figures.clickCount;
                m.artifactAligned[ch] = figures.locked;
            }
        }

        m.correlation = 0.0f;

        m.dropoutFlag    = intervalDropoutSeen ? 1 : 0;
        m.clipCountBlock = intervalClipCount;
        m.statusCode     = intervalStatusBits | (intervalDropoutSeen ? StatusBits::dropoutActive : 0);

        measurementQueue.push (m);

        // Reset interval accumulators.
        samplesIntoInterval = 0;

        for (int ch = 0; ch < maxChannels; ++ch)
        {
            refSumSquares[ch]   = 0.0;
            inSumSquares[ch]    = 0.0;
            refIntervalPeak[ch] = 0.0f;
            inIntervalPeak[ch]  = 0.0f;
            intervalClipCountPerChannel[ch] = 0;
        }

        intervalClipCount    = 0;
        intervalStatusBits   = StatusBits::ok;
        intervalDropoutSeen  = false;
    }

    return stats;
}

//==============================================================================
void AnalysisProcessor::noteDropoutActive() noexcept
{
    intervalDropoutSeen = true;
    intervalStatusBits |= StatusBits::dropoutActive;
}

//==============================================================================
void AnalysisProcessor::resetPeakHold() noexcept
{
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        atomicRefPeakHoldDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
        atomicInPeakHoldDb[ch].store (silenceFloorDb, std::memory_order_relaxed);
    }
}

double AnalysisProcessor::getElapsedSeconds() const noexcept
{
    return atomicElapsedSeconds.load (std::memory_order_relaxed);
}

} // namespace pnam
