/*
  ==============================================================================
    ReferenceHistory.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "ReferenceHistory.h"

namespace pnam
{

namespace
{
    inline int nextPowerOfTwo (int value) noexcept
    {
        int result = 1;
        while (result < value)
            result <<= 1;
        return result;
    }
}

//==============================================================================
void ReferenceHistory::prepare (int numChannels, int capacitySamples)
{
    numChans = juce::jmax (1, numChannels);
    capacity = nextPowerOfTwo (juce::jmax (64, capacitySamples));
    indexMask = capacity - 1;

    ring.setSize (numChans, capacity, false, true, true);
    reset();
}

void ReferenceHistory::reset() noexcept
{
    if (ring.getNumSamples() > 0)
        ring.clear();

    totalWritten.store (0, std::memory_order_release);
}

//==============================================================================
void ReferenceHistory::write (const float* const* data, int numCh, int numSamples) noexcept
{
    if (numSamples <= 0 || numChans <= 0 || ring.getNumSamples() == 0 || data == nullptr)
        return;

    const int64_t startPos = totalWritten.load (std::memory_order_relaxed);

    for (int ch = 0; ch < numChans; ++ch)
    {
        const int srcCh = (numCh > 0) ? juce::jlimit (0, numCh - 1, ch) : -1;
        const float* src = (srcCh >= 0) ? data[srcCh] : nullptr;
        float* ringData = ring.getWritePointer (ch);

        for (int i = 0; i < numSamples; ++i)
        {
            const int idx = (int) ((startPos + (int64_t) i) & (int64_t) indexMask);
            ringData[idx] = (src != nullptr) ? src[i] : 0.0f;
        }
    }

    totalWritten.store (startPos + numSamples, std::memory_order_release);
}

//==============================================================================
int ReferenceHistory::readLatest (float* dest, int channel, int numSamples, int delaySamples) const noexcept
{
    if (dest == nullptr || numSamples <= 0)
        return 0;

    if (ring.getNumSamples() == 0 || numChans <= 0)
    {
        juce::FloatVectorOperations::clear (dest, numSamples);
        return 0;
    }

    channel = juce::jlimit (0, numChans - 1, channel);
    const float* ringData = ring.getReadPointer (channel);

    const int64_t written       = totalWritten.load (std::memory_order_acquire);
    const int64_t endExclusive  = written - (int64_t) juce::jmax (0, delaySamples);

    int validCount = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        const int64_t srcPos = endExclusive - (int64_t) numSamples + (int64_t) i;

        const bool inRange = srcPos >= 0
                           && srcPos < written
                           && (written - srcPos) <= (int64_t) capacity;

        if (inRange)
        {
            const int idx = (int) (srcPos & (int64_t) indexMask);
            dest[i] = ringData[idx];
            ++validCount;
        }
        else
        {
            dest[i] = 0.0f;
        }
    }

    return validCount;
}

} // namespace pnam
