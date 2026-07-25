#pragma once
/*
  ==============================================================================
    PinkNoiseGenerator.h
    Pink Noise Audio Monitor (pnam) — deterministic seeded pink-noise source.

    Implementation contract (for the .cpp author):
      - PRNG: xorshift64* seeded via setSeed(). Never use juce::Random or
        std::random_device at runtime — the whole point is that the same seed
        produces the same sample stream, so a session can be replayed exactly.
      - Pink filter: Paul Kellet "economy" 3-pole pinking filter applied to the
        white PRNG stream, one independent filter state (and PRNG stream) per
        channel in IndependentStereo mode; a single shared stream copied to
        all channels in LinkedMono mode.
      - Determinism: given the same (seed, stereoMode, channel count) the
        pre-gain sample stream is bit-identical across runs and machines
        (no platform-dependent math). Level/fade/mute are applied AFTER the
        deterministic stream, as a smoothed gain.
      - Gain: setLevelDb() targets are applied through a short linear smoother
        (~50 ms) to avoid zipper noise. Fades (startFade) use an independent
        longer ramp (setFadeTimeMs, default 500 ms) multiplied on top.
        Mute is a fade to zero using the fade ramp.
      - Output headroom: the pink filter output is normalised so that at
        0 dBFS setting, peaks stay below 1.0 (Kellet filter gain compensated
        by a fixed constant, e.g. 0.11 as in the reference implementation).

    Threading:
      - prepare()/reset()/setSeed(): NON-REAL-TIME, audio stopped.
      - setLevelDb / setMuted / setStereoMode / startFade: any thread
        (they only store into atomics; the audio thread picks targets up at
        block boundaries).
      - processBlock(): AUDIO THREAD. Never allocates, locks, or blocks.
  ==============================================================================
*/

#include <atomic>
#include <cstdint>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "ReferenceHistory.h"

namespace pnam
{

//==============================================================================
class PinkNoiseGenerator
{
public:
    /** Channel behaviour. */
    enum class StereoMode : int
    {
        LinkedMono,        ///< One noise stream, duplicated to every output channel.
        IndependentStereo  ///< Decorrelated per-channel streams (seed, seed+1, ...).
    };

    /** Default output level, dBFS. */
    static constexpr double defaultLevelDb = -20.0;

    /** Default fade in/out duration, milliseconds. */
    static constexpr double defaultFadeTimeMs = 500.0;

    PinkNoiseGenerator() = default;

    PinkNoiseGenerator (const PinkNoiseGenerator&) = delete;
    PinkNoiseGenerator& operator= (const PinkNoiseGenerator&) = delete;

    //==========================================================================
    // Configuration (non-real-time unless noted)

    /** NON-REAL-TIME. Sets the PRNG seed and resets the noise/filter state so
        the stream restarts from sample 0 of that seed. A seed of 0 is
        remapped internally to a fixed non-zero constant (xorshift requirement). */
    void setSeed (uint64_t seed);

    /** Seed currently in use (post zero-remap). */
    uint64_t getSeed() const noexcept;

    /** NON-REAL-TIME. Must be called before processBlock, and again whenever
        the device sample rate or maximum block size changes. Resets fade and
        gain smoothers; does NOT reset the seed/stream (call setSeed for that). */
    void prepare (double sampleRate, int maximumBlockSize);

    /** NON-REAL-TIME. Restarts the deterministic stream from sample 0 of the
        current seed and clears filter/fade state. */
    void reset();

    //==========================================================================
    // Live parameters (thread-safe: atomic targets, smoothed on audio thread)

    /** Target output level in dBFS (applied via ~50 ms smoothing).
        Values are clamped to [-120, 0]. Default -20 dBFS. */
    void setLevelDb (double levelDbfs) noexcept;
    double getLevelDb() const noexcept;

    /** true -> ramps to silence over the fade time; false -> ramps back up.
        The deterministic stream keeps advancing while muted, so the
        sample-position <-> time mapping is preserved. */
    void setMuted (bool shouldBeMuted) noexcept;
    bool isMuted() const noexcept;

    void setStereoMode (StereoMode mode) noexcept;
    StereoMode getStereoMode() const noexcept;

    /** Begins a fade. startFade(true) fades in from silence (used at audio
        start), startFade(false) fades out (used before stopping the device so
        stopping never clicks). */
    void startFade (bool fadeIn) noexcept;

    /** Fade in/out duration in milliseconds (default 500). Any thread. */
    void setFadeTimeMs (double ms) noexcept;

    /** True while a fade ramp (either direction) is still moving. The
        AudioEngine polls this after startFade(false) to know when it is safe
        to stop the device. */
    bool isFadeActive() const noexcept;

    /** Combined instantaneous gain (level * fade * mute), linear [0..1].
        Atomic; used by AnalysisProcessor / EventDetector to suppress
        comparisons during fades/mute, and by the UI. */
    float getCurrentGainLinear() const noexcept;

    //==========================================================================
    // Reference retention

    /** Optional: attach a ReferenceHistory; when set, processBlock() writes
        each generated (post-gain) block into it before returning. Set once
        during setup, before audio starts; pass nullptr to detach. */
    void setReferenceHistory (ReferenceHistory* historyOrNull) noexcept;

    //==========================================================================
    // Audio

    /** AUDIO THREAD. Fills out[ch][0..numSamples) for ch in [0, numChannels)
        with post-gain pink noise. numSamples <= maximumBlockSize from
        prepare(). Also writes the block into the attached ReferenceHistory
        (if any) and updates the last-block copy for getLastBlockReference().
        Never allocates, locks, or blocks. */
    void processBlock (float* const* out, int numChannels, int numSamples) noexcept;

    //==========================================================================
    /** AUDIO THREAD (same callback). View of the exact samples produced by the
        most recent processBlock() call for one channel — valid until the next
        processBlock(). For cross-thread access use the ReferenceHistory
        instead. Returns nullptr if processBlock has not run since prepare().
        @param numSamplesOut  receives the sample count of the last block. */
    const float* getLastBlockReference (int channel, int& numSamplesOut) const noexcept;

    /** Total samples generated per channel since reset() (audio-thread clock). */
    int64_t getTotalSamplesGenerated() const noexcept;

private:
    //==========================================================================
    // Shared with the FIFO payload ceiling so the generator can feed every
    // output channel the device exposes (see DataModel.h).
    static constexpr int maxChannels = pnam::maxChannels;

    // Deterministic core (audio thread only after setup)
    uint64_t prngState[maxChannels] {};
    float    kelletState[maxChannels][3] {};
    int64_t  samplesGenerated { 0 };
    uint64_t seedInUse { 0 };

    // Parameter targets (any thread) -> smoothed on audio thread
    std::atomic<float> targetLevelLinear { 0.1f };   // -20 dBFS
    std::atomic<bool>  muted             { false };
    std::atomic<int>   stereoMode        { (int) StereoMode::LinkedMono };
    std::atomic<float> fadeTimeMsAtomic  { (float) defaultFadeTimeMs };
    std::atomic<int>   pendingFade       { 0 };      // 0 none, +1 in, -1 out
    std::atomic<float> currentGainLinear { 0.0f };
    std::atomic<bool>  fadeActive        { false };

    // Smoothers (audio thread only)
    juce::SmoothedValue<float> levelSmoother;
    juce::SmoothedValue<float> fadeSmoother;

    // Retained last block + optional history
    juce::AudioBuffer<float>        lastBlock;      // preallocated in prepare()
    int                             lastBlockSamples { 0 };
    std::atomic<ReferenceHistory*>  referenceHistory { nullptr };

    double sampleRate { 0.0 };
    int    maxBlock   { 0 };
};

} // namespace pnam
