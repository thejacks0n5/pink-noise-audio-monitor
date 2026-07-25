#pragma once
/*
  ==============================================================================
    ReferenceHistory.h
    Pink Noise Audio Monitor (pnam) — retained reference-signal ring buffer.

    A preallocated per-channel ring buffer holding the most recent reference
    (generated pink-noise) samples, so the analysis stage can compare the
    returned input against exactly what was sent, including at a delay offset
    (round-trip latency) in later versions.

    Threading model (V1):
      - WRITER: the audio thread, once per callback, immediately after
        PinkNoiseGenerator::processBlock() (post-gain/fade — the buffer holds
        the exact samples handed to the output device).
      - READER: also the audio thread (AnalysisProcessor::processBlock runs in
        the same callback, after write()). Same-thread access needs no
        synchronisation; the atomic write counter exists so a message-thread
        reader (e.g. the waveform display) can snapshot recent samples with
        only benign tearing (a torn waveform pixel is acceptable; a lock or
        allocation on the audio thread is not).

    No locks, no allocation after prepare(). prepare() must be called from a
    non-real-time thread while audio is stopped.
  ==============================================================================
*/

#include <atomic>
#include <cstdint>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

namespace pnam
{

//==============================================================================
class ReferenceHistory
{
public:
    ReferenceHistory() = default;

    ReferenceHistory (const ReferenceHistory&) = delete;
    ReferenceHistory& operator= (const ReferenceHistory&) = delete;

    //==========================================================================
    /** NON-REAL-TIME. Allocates the ring storage.
        @param numChannels      1 or 2.
        @param capacitySamples  ring length per channel; must comfortably exceed
                                the largest expected round-trip latency plus one
                                block. Recommended: at least 2 seconds of audio
                                (e.g. (int) (sampleRate * 2.0)). Rounded up to a
                                power of two internally for cheap wrapping. */
    void prepare (int numChannels, int capacitySamples);

    /** NON-REAL-TIME. Zeroes the ring and resets the write counter to 0.
        Call between sessions while audio is stopped. */
    void reset() noexcept;

    //==========================================================================
    /** AUDIO THREAD. Appends one block of reference samples (all channels).
        `data[ch]` must be valid for ch in [0, numCh). If numCh is less than
        the prepared channel count, the last provided channel is duplicated
        into the remaining ring channels (mono -> stereo fan-out).
        Never allocates, locks, or blocks. */
    void write (const float* const* data, int numCh, int numSamples) noexcept;

    //==========================================================================
    /** AUDIO THREAD (V1). Copies the most recent `numSamples` samples of one
        channel into `dest`, ending `delaySamples` before the newest sample
        (delaySamples = 0 -> the very latest samples, i.e. the block just
        written). If fewer samples have ever been written, the missing leading
        portion of `dest` is zero-filled.
        @returns the number of valid (non-zero-filled) samples copied. */
    int readLatest (float* dest, int channel, int numSamples,
                    int delaySamples = 0) const noexcept;

    //==========================================================================
    /** Total samples written per channel since reset (monotonic). Atomic so a
        message-thread waveform view can align its own reads. */
    int64_t getTotalSamplesWritten() const noexcept
    {
        return totalWritten.load (std::memory_order_acquire);
    }

    int getNumChannels() const noexcept     { return numChans; }

    /** Actual (power-of-two) ring capacity per channel, in samples. */
    int getCapacitySamples() const noexcept { return capacity; }

private:
    juce::AudioBuffer<float> ring;              // preallocated in prepare()
    int                      numChans   { 0 };
    int                      capacity   { 0 };  // power of two
    int                      indexMask  { 0 };  // capacity - 1
    std::atomic<int64_t>     totalWritten { 0 };
};

} // namespace pnam
