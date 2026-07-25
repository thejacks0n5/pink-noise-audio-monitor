/*
  ==============================================================================
    AnalysisProcessor.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "AnalysisProcessor.h"
#include <cmath>

namespace pnam
{

namespace
{
    /** Largest block this processor will analyse in one call. Reference
        samples are copied into fixed-size stack buffers of this length so
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
    atomicInPeakDb[0].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInPeakDb[1].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInRmsDb[0].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInRmsDb[1].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInPeakHoldDb[0].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInPeakHoldDb[1].store (silenceFloorDb, std::memory_order_relaxed);
}

//==============================================================================
void AnalysisProcessor::prepare (double newSampleRate, int maximumBlockSize, int newNumInputChannels)
{
    // Fold whatever the previous device configuration accumulated into the
    // session offset BEFORE adopting the new sample rate, so the clock keeps
    // moving forward across a device restart instead of rewinding to zero.
    if (sampleRate > 0.0 && samplesProcessed > 0)
        elapsedOffsetSeconds += (double) samplesProcessed / sampleRate;

    sampleRate = newSampleRate;
    maxBlock   = maximumBlockSize;
    numInCh    = juce::jlimit (0, 2, newNumInputChannels);

    samplesProcessed    = 0;
    samplesIntoInterval = 0;

    refSumSquares       = 0.0;
    inSumSquares[0]      = 0.0;
    inSumSquares[1]      = 0.0;
    refIntervalPeak      = 0.0f;
    inIntervalPeak[0]    = 0.0f;
    inIntervalPeak[1]    = 0.0f;
    intervalClipCount    = 0;
    intervalStatusBits   = StatusBits::ok;
    intervalDropoutSeen  = false;

    atomicRefPeakDb.store (silenceFloorDb, std::memory_order_relaxed);
    atomicRefRmsDb.store (silenceFloorDb, std::memory_order_relaxed);
    atomicInPeakDb[0].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInPeakDb[1].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInRmsDb[0].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInRmsDb[1].store (silenceFloorDb, std::memory_order_relaxed);
    atomicRefPeakHoldDb.store (silenceFloorDb, std::memory_order_relaxed);
    atomicInPeakHoldDb[0].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInPeakHoldDb[1].store (silenceFloorDb, std::memory_order_relaxed);
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

    const int samplesToUse = juce::jlimit (0, maxAnalysedBlockSamples, juce::jmin (numSamples, maxBlock > 0 ? maxBlock : numSamples));

    // Fixed-size scratch for the reference read-back — static storage so no
    // large stack frame or heap allocation happens on every callback.
    static thread_local float refScratchL[maxAnalysedBlockSamples];
    static thread_local float refScratchR[maxAnalysedBlockSamples];

    //--------------------------------------------------------------------
    // Reference: read the newest samples just written this callback.
    const int refChannels = juce::jmax (1, reference.getNumChannels());

    reference.readLatest (refScratchL, 0, samplesToUse, 0);
    float refPeakL = 0.0f;
    double refSumSquaresL = 0.0;
    analyseChannel (refScratchL, samplesToUse, refPeakL, refSumSquaresL);

    float   blockRefPeakLinear = refPeakL;
    double  blockRefSumSquares = refSumSquaresL;

    if (refChannels > 1)
    {
        reference.readLatest (refScratchR, 1, samplesToUse, 0);
        float refPeakR = 0.0f;
        double refSumSquaresR = 0.0;
        analyseChannel (refScratchR, samplesToUse, refPeakR, refSumSquaresR);

        blockRefPeakLinear = juce::jmax (blockRefPeakLinear, refPeakR);
        blockRefSumSquares += refSumSquaresR;
    }

    const float refPeakDbBlock = linearToDb (blockRefPeakLinear);
    const float refRmsDbBlock  = linearToDb ((float) std::sqrt (blockRefSumSquares
                                              / (double) juce::jmax (1, samplesToUse * refChannels)));

    stats.refPeakDb = refPeakDbBlock;
    stats.refRmsDb  = refRmsDbBlock;

    atomicRefPeakDb.store (refPeakDbBlock, std::memory_order_relaxed);
    atomicRefRmsDb.store (refRmsDbBlock, std::memory_order_relaxed);

    if (refPeakDbBlock > atomicRefPeakHoldDb.load (std::memory_order_relaxed))
        atomicRefPeakHoldDb.store (refPeakDbBlock, std::memory_order_relaxed);

    //--------------------------------------------------------------------
    // Input channels: [0]=L, [1]=R (mono mirrors L into R).
    const float clipThreshold = clipThresholdLinear.load (std::memory_order_relaxed);
    int32_t blockClipCount = 0;

    float  inPeakLinear[2] { 0.0f, 0.0f };
    double inSumSquaresBlock[2] { 0.0, 0.0 };

    const int channelsToAnalyse = juce::jlimit (0, 2, numInputChannelsThisBlock);

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

        for (int i = 0; i < samplesToUse; ++i)
            if (std::abs (data[i]) >= clipThreshold)
                ++blockClipCount;
    }

    if (channelsToAnalyse == 1)
    {
        // Mono: mirror channel 0 into channel 1.
        inPeakLinear[1] = inPeakLinear[0];
        inSumSquaresBlock[1] = inSumSquaresBlock[0];
    }

    const float inPeakDbBlock[2] = { linearToDb (inPeakLinear[0]), linearToDb (inPeakLinear[1]) };
    const float inRmsDbBlock[2]  =
    {
        linearToDb ((float) std::sqrt (inSumSquaresBlock[0] / (double) juce::jmax (1, samplesToUse))),
        linearToDb ((float) std::sqrt (inSumSquaresBlock[1] / (double) juce::jmax (1, samplesToUse)))
    };

    stats.inPeakDb[0] = inPeakDbBlock[0];
    stats.inPeakDb[1] = inPeakDbBlock[1];
    stats.inRmsDb[0]  = inRmsDbBlock[0];
    stats.inRmsDb[1]  = inRmsDbBlock[1];
    stats.clipCount   = blockClipCount;
    stats.gainDiffDb  = juce::jmax (inRmsDbBlock[0], inRmsDbBlock[1]) - refRmsDbBlock;

    for (int ch = 0; ch < 2; ++ch)
    {
        atomicInPeakDb[ch].store (inPeakDbBlock[ch], std::memory_order_relaxed);
        atomicInRmsDb[ch].store (inRmsDbBlock[ch], std::memory_order_relaxed);

        if (inPeakDbBlock[ch] > atomicInPeakHoldDb[ch].load (std::memory_order_relaxed))
            atomicInPeakHoldDb[ch].store (inPeakDbBlock[ch], std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------
    // Interval accumulation for the next Measurement push.
    refSumSquares          += blockRefSumSquares;
    inSumSquares[0]         += inSumSquaresBlock[0];
    inSumSquares[1]         += inSumSquaresBlock[1];
    refIntervalPeak          = juce::jmax (refIntervalPeak, blockRefPeakLinear);
    inIntervalPeak[0]        = juce::jmax (inIntervalPeak[0], inPeakLinear[0]);
    inIntervalPeak[1]        = juce::jmax (inIntervalPeak[1], inPeakLinear[1]);
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
        m.bufferSize     = numSamples;

        m.refPeakDb = linearToDb (refIntervalPeak);
        m.refRmsDb  = linearToDb ((float) std::sqrt (refSumSquares / (double) (samplesForAverage * juce::jmax (1, refChannels))));

        m.inPeakDbL = linearToDb (inIntervalPeak[0]);
        m.inPeakDbR = linearToDb (inIntervalPeak[1]);
        m.inRmsDbL  = linearToDb ((float) std::sqrt (inSumSquares[0] / (double) samplesForAverage));
        m.inRmsDbR  = linearToDb ((float) std::sqrt (inSumSquares[1] / (double) samplesForAverage));

        m.gainDiffDb = juce::jmax (m.inRmsDbL, m.inRmsDbR) - m.refRmsDb;
        m.correlation = 0.0f;

        m.dropoutFlag    = intervalDropoutSeen ? 1 : 0;
        m.clipCountBlock = intervalClipCount;
        m.statusCode     = intervalStatusBits | (intervalDropoutSeen ? StatusBits::dropoutActive : 0);

        measurementQueue.push (m);

        // Reset interval accumulators.
        samplesIntoInterval = 0;
        refSumSquares       = 0.0;
        inSumSquares[0]      = 0.0;
        inSumSquares[1]      = 0.0;
        refIntervalPeak      = 0.0f;
        inIntervalPeak[0]    = 0.0f;
        inIntervalPeak[1]    = 0.0f;
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
    atomicRefPeakHoldDb.store (silenceFloorDb, std::memory_order_relaxed);
    atomicInPeakHoldDb[0].store (silenceFloorDb, std::memory_order_relaxed);
    atomicInPeakHoldDb[1].store (silenceFloorDb, std::memory_order_relaxed);
}

double AnalysisProcessor::getElapsedSeconds() const noexcept
{
    return atomicElapsedSeconds.load (std::memory_order_relaxed);
}

} // namespace pnam
