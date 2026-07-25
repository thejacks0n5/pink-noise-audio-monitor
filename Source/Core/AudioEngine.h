#pragma once
/*
  ==============================================================================
    AudioEngine.h
    Pink Noise Audio Monitor (pnam) — audio-thread glue class.

    NOTE: This header did not exist as part of the fixed contract handed to
    this engineer; it is authored here strictly to match the "Expected
    AudioEngine surface" documented verbatim in Source/UI/MainComponent.h and
    the responsibilities described in ARCHITECTURE.md §5. No signature here
    was invented beyond what those two documents already specify; anything
    additional (private members, helper methods) is an implementation detail
    behind that public surface.

    Owns: juce::AudioDeviceManager, PinkNoiseGenerator, ReferenceHistory (ref
    + a second ring for input, so the UI can pull waveform data for both),
    AnalysisProcessor, EventDetector. Registers itself as the
    juce::AudioIODeviceCallback and runs the ARCHITECTURE.md §4 per-callback
    sequence entirely on the audio thread: never allocates, locks, or does
    file/UI work inside audioDeviceIOCallbackWithContext.
  ==============================================================================
*/

#include <atomic>
#include <cstdint>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "../Model/DataModel.h"
#include "MeasurementQueue.h"
#include "PinkNoiseGenerator.h"
#include "ReferenceHistory.h"
#include "AnalysisProcessor.h"
#include "EventDetector.h"

namespace pnam
{

//==============================================================================
class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    /** @param measurementQueueToUse  audio->message measurement FIFO (owned by
            the caller, e.g. MainComponent; must outlive this object).
        @param eventQueueToUse        audio->message event FIFO (same lifetime
            requirement). */
    AudioEngine (MeasurementQueue& measurementQueueToUse, EventQueue& eventQueueToUse);
    ~AudioEngine() override;

    AudioEngine (const AudioEngine&) = delete;
    AudioEngine& operator= (const AudioEngine&) = delete;

    //==========================================================================
    /** MESSAGE THREAD. Opens the audio device (best-effort stereo, falling
        back to mono), registers this as the device callback, and starts the
        generator's fade-in. @returns false if no device could be opened. */
    bool start();

    /** MESSAGE THREAD. Fades the generator out, waits (bounded) for the fade
        to complete, then stops/detaches the device. Safe to call when not
        running. */
    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    AnalysisProcessor&  getAnalysis()  noexcept { return analysisProcessor; }
    EventDetector&      getDetector()  noexcept { return eventDetector; }
    PinkNoiseGenerator& getGenerator() noexcept { return generator; }

    double getSampleRate() const noexcept          { return currentSampleRate.load (std::memory_order_relaxed); }
    int    getBufferSize() const noexcept          { return currentBufferSize.load (std::memory_order_relaxed); }
    int    getNumInputChannels() const noexcept    { return currentNumInputChannels.load (std::memory_order_relaxed); }
    int    getNumOutputChannels() const noexcept   { return currentNumOutputChannels.load (std::memory_order_relaxed); }

    juce::String getInputDeviceName() const;
    juce::String getOutputDeviceName() const;

    //==========================================================================
    /** MESSAGE THREAD. Copies up to maxSamples of the most recent reference
        (generated) samples into dest. @returns the number of valid samples. */
    int readReferenceWaveform (float* dest, int maxSamples);

    /** MESSAGE THREAD. Copies up to maxSamples of the most recent input
        samples (channel 0 = L, 1 = R) into dest. @returns valid sample count. */
    int readInputWaveform (int channel, float* dest, int maxSamples);

private:
    //==========================================================================
    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                            int numInputChannels,
                                            float* const* outputChannelData,
                                            int numOutputChannels,
                                            int numSamples,
                                            const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;

    //==========================================================================
    MeasurementQueue& measurementQueue;
    EventQueue&       eventQueue;

    juce::AudioDeviceManager deviceManager;

    PinkNoiseGenerator generator;
    ReferenceHistory   referenceHistory; // reference (generated) ring
    ReferenceHistory   inputHistory;     // input ring, for waveform display only
    AnalysisProcessor  analysisProcessor;
    EventDetector      eventDetector;

    std::atomic<bool>   running { false };

    std::atomic<double> currentSampleRate        { 0.0 };
    std::atomic<int>    currentBufferSize        { 0 };
    std::atomic<int>    currentNumInputChannels  { 0 };
    std::atomic<int>    currentNumOutputChannels { 0 };

    juce::CriticalSection nameLock;
    juce::String inputDeviceName, outputDeviceName;

    // Audio-thread-only state (no synchronisation needed: written/read only
    // from within the IO callback / audioDeviceAboutToStart, never message
    // thread).
    float   previousGainLinear   { 0.0f };
    int64_t samplesSinceStart    { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace pnam
