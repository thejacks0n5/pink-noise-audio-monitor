/*
  ==============================================================================
    PinkNoiseGenerator.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "PinkNoiseGenerator.h"

namespace pnam
{

namespace
{
    /** xorshift64* — cheap, deterministic, decent statistical quality. */
    inline uint64_t nextXorshift64Star (uint64_t& state) noexcept
    {
        uint64_t x = state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state = x;
        return x * 0x2545F4914F6CDD1DULL;
    }

    /** Top 24 bits of the xorshift output -> a white sample in [-1, 1). */
    inline float toWhiteSample (uint64_t raw) noexcept
    {
        const uint32_t bits = (uint32_t) (raw >> 40) & 0x00FFFFFFu; // 24 bits
        return (float) bits * (1.0f / 8388608.0f) - 1.0f;           // / 2^23 - 1
    }

    constexpr uint64_t zeroSeedReplacement = 0x9E3779B97F4A7C15ULL; // golden-ratio constant, never 0
}

//==============================================================================
void PinkNoiseGenerator::setSeed (uint64_t seed)
{
    seedInUse = (seed == 0) ? zeroSeedReplacement : seed;

    prngState[0] = seedInUse;

    // Decorrelated stream for the second channel (IndependentStereo mode).
    uint64_t secondSeed = seedInUse ^ 0xBF58476D1CE4E5B9ULL;
    if (secondSeed == 0)
        secondSeed = 0xD1B54A32D192ED03ULL;
    prngState[1] = secondSeed;

    for (int ch = 0; ch < maxChannels; ++ch)
        kelletState[ch][0] = kelletState[ch][1] = kelletState[ch][2] = 0.0f;

    samplesGenerated = 0;
}

uint64_t PinkNoiseGenerator::getSeed() const noexcept
{
    return seedInUse;
}

void PinkNoiseGenerator::prepare (double sampleRateIn, int maximumBlockSize)
{
    sampleRate = sampleRateIn;
    maxBlock   = juce::jmax (1, maximumBlockSize);

    lastBlock.setSize (maxChannels, maxBlock, false, true, true);
    lastBlockSamples = 0;

    levelSmoother.reset (sampleRate, 0.05); // ~50 ms level smoothing
    levelSmoother.setCurrentAndTargetValue (targetLevelLinear.load (std::memory_order_relaxed));

    const double fadeSeconds = juce::jmax (0.001, fadeTimeMsAtomic.load (std::memory_order_relaxed) / 1000.0);
    fadeSmoother.reset (sampleRate, fadeSeconds);
    fadeSmoother.setCurrentAndTargetValue (0.0f); // start silent; AudioEngine calls startFade(true)

    currentGainLinear.store (0.0f, std::memory_order_relaxed);
    fadeActive.store (false, std::memory_order_relaxed);
    pendingFade.store (0, std::memory_order_relaxed);

    if (seedInUse == 0)
        setSeed (0); // establishes the zero-remapped default seed

    reset();
}

void PinkNoiseGenerator::reset()
{
    setSeed (seedInUse);

    lastBlockSamples = 0;
    if (lastBlock.getNumSamples() > 0)
        lastBlock.clear();

    levelSmoother.setCurrentAndTargetValue (targetLevelLinear.load (std::memory_order_relaxed));
    fadeSmoother.setCurrentAndTargetValue (fadeSmoother.getCurrentValue());
    currentGainLinear.store (0.0f, std::memory_order_relaxed);
    fadeActive.store (false, std::memory_order_relaxed);
}

//==============================================================================
void PinkNoiseGenerator::setLevelDb (double levelDbfs) noexcept
{
    const double clamped = juce::jlimit (-120.0, 0.0, levelDbfs);
    targetLevelLinear.store ((float) juce::Decibels::decibelsToGain (clamped, -120.0), std::memory_order_relaxed);
}

double PinkNoiseGenerator::getLevelDb() const noexcept
{
    return juce::Decibels::gainToDecibels ((double) targetLevelLinear.load (std::memory_order_relaxed), -120.0);
}

void PinkNoiseGenerator::setMuted (bool shouldBeMuted) noexcept
{
    muted.store (shouldBeMuted, std::memory_order_relaxed);
    pendingFade.store (shouldBeMuted ? -1 : 1, std::memory_order_relaxed);
}

bool PinkNoiseGenerator::isMuted() const noexcept
{
    return muted.load (std::memory_order_relaxed);
}

void PinkNoiseGenerator::setStereoMode (StereoMode mode) noexcept
{
    stereoMode.store ((int) mode, std::memory_order_relaxed);
}

PinkNoiseGenerator::StereoMode PinkNoiseGenerator::getStereoMode() const noexcept
{
    return (StereoMode) stereoMode.load (std::memory_order_relaxed);
}

void PinkNoiseGenerator::startFade (bool fadeIn) noexcept
{
    pendingFade.store (fadeIn ? 1 : -1, std::memory_order_relaxed);
}

void PinkNoiseGenerator::setFadeTimeMs (double ms) noexcept
{
    fadeTimeMsAtomic.store ((float) juce::jmax (1.0, ms), std::memory_order_relaxed);
}

bool PinkNoiseGenerator::isFadeActive() const noexcept
{
    return fadeActive.load (std::memory_order_relaxed);
}

float PinkNoiseGenerator::getCurrentGainLinear() const noexcept
{
    return currentGainLinear.load (std::memory_order_relaxed);
}

void PinkNoiseGenerator::setReferenceHistory (ReferenceHistory* historyOrNull) noexcept
{
    referenceHistory.store (historyOrNull, std::memory_order_release);
}

//==============================================================================
void PinkNoiseGenerator::processBlock (float* const* out, int numChannels, int numSamples) noexcept
{
    if (out == nullptr || numChannels <= 0 || numSamples <= 0 || sampleRate <= 0.0)
        return;

    const int blockSamples = juce::jmin (numSamples, maxBlock);

    // --- Adopt live parameter targets at this block boundary -------------
    levelSmoother.setTargetValue (targetLevelLinear.load (std::memory_order_relaxed));

    const int pending = pendingFade.exchange (0, std::memory_order_acq_rel);
    if (pending != 0)
    {
        const double fadeSeconds = juce::jmax (0.001, fadeTimeMsAtomic.load (std::memory_order_relaxed) / 1000.0);
        fadeSmoother.reset (sampleRate, fadeSeconds);
        fadeSmoother.setTargetValue (pending > 0 ? 1.0f : 0.0f);
    }

    const bool independent = (StereoMode) stereoMode.load (std::memory_order_relaxed) == StereoMode::IndependentStereo;
    const int  channelsToGenerate = juce::jmin (numChannels, maxChannels);

    // --- Advance the combined level*fade gain envelope once per sample,
    //     into a fixed-size stack scratch buffer, so every channel applies
    //     the identical gain trajectory for this block (no heap allocation;
    //     capped at gainScratchCapacity, matching typical device block sizes).
    constexpr int gainScratchCapacity = 8192;
    float gainScratch[gainScratchCapacity];
    const int gainSamples = juce::jmin (blockSamples, gainScratchCapacity);

    for (int i = 0; i < gainSamples; ++i)
    {
        const float levelGain = levelSmoother.getNextValue();
        const float fadeGain  = fadeSmoother.getNextValue();
        gainScratch[i] = levelGain * fadeGain;
    }

    float lastGainThisBlock = gainSamples > 0 ? gainScratch[gainSamples - 1]
                                              : currentGainLinear.load (std::memory_order_relaxed);

    for (int ch = 0; ch < channelsToGenerate; ++ch)
    {
        const int streamIdx = independent ? ch : 0;
        float* dest     = out[ch];
        float* lastDest = lastBlock.getWritePointer (ch);

        if (dest == nullptr)
            continue;

        uint64_t& prng = prngState[streamIdx];
        float& b0 = kelletState[streamIdx][0];
        float& b1 = kelletState[streamIdx][1];
        float& b2 = kelletState[streamIdx][2];

        for (int i = 0; i < gainSamples; ++i)
        {
            const uint64_t raw = nextXorshift64Star (prng);
            const float white = toWhiteSample (raw);

            b0 = 0.99765f * b0 + white * 0.0990460f;
            b1 = 0.96300f * b1 + white * 0.2965164f;
            b2 = 0.57000f * b2 + white * 1.0526913f;
            const float pink = (b0 + b1 + b2 + white * 0.1848f) * 0.11f;

            const float sample = pink * gainScratch[i];
            dest[i]     = sample;
            lastDest[i] = sample;
        }

        for (int i = gainSamples; i < numSamples; ++i)
            dest[i] = 0.0f;
    }

    for (int ch = channelsToGenerate; ch < numChannels; ++ch)
        if (out[ch] != nullptr)
            juce::FloatVectorOperations::clear (out[ch], numSamples);

    currentGainLinear.store (lastGainThisBlock, std::memory_order_relaxed);
    fadeActive.store (fadeSmoother.isSmoothing(), std::memory_order_relaxed);

    lastBlockSamples = gainSamples;
    samplesGenerated += gainSamples;

    if (auto* history = referenceHistory.load (std::memory_order_acquire))
        history->write (out, channelsToGenerate, gainSamples);
}

const float* PinkNoiseGenerator::getLastBlockReference (int channel, int& numSamplesOut) const noexcept
{
    numSamplesOut = lastBlockSamples;

    if (channel < 0 || channel >= maxChannels || lastBlockSamples <= 0)
        return nullptr;

    return lastBlock.getReadPointer (channel);
}

int64_t PinkNoiseGenerator::getTotalSamplesGenerated() const noexcept
{
    return samplesGenerated;
}

} // namespace pnam
