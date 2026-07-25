/*
  ==============================================================================
    AudioEngine.cpp
    Pink Noise Audio Monitor (pnam)

    Implements the ARCHITECTURE.md §4/§5 per-callback sequence:
      1. PinkNoiseGenerator::processBlock -> output, post-gain block also
         written into ReferenceHistory (inside the generator).
      2. Build statusBits (outputMuted / fadingIn / fadingOut / settling /
         inputInactive) from generator + engine state.
      3. AnalysisProcessor::processBlock -> BlockStats, pushes Measurement
         every interval.
      4. EventDetector::processBlockStats -> dropoutActive; if true,
         AnalysisProcessor::noteDropoutActive().

    Never allocates, locks, or does file/UI work inside the IO callback.
  ==============================================================================
*/

#include "AudioEngine.h"

namespace pnam
{

//==============================================================================
AudioEngine::AudioEngine (MeasurementQueue& measurementQueueToUse, EventQueue& eventQueueToUse)
    : measurementQueue (measurementQueueToUse),
      eventQueue (eventQueueToUse),
      analysisProcessor (measurementQueueToUse),
      eventDetector (eventQueueToUse)
{
}

AudioEngine::~AudioEngine()
{
    stop();
}

//==============================================================================
bool AudioEngine::start()
{
    if (running.load (std::memory_order_relaxed))
        return true;

    // Best-effort: try stereo in/out first, then fall back to mono.
    juce::String error = deviceManager.initialiseWithDefaultDevices (2, 2);

    if (error.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr)
        error = deviceManager.initialiseWithDefaultDevices (1, 1);

    if (error.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr)
        return false;

    samplesSinceStart = 0;
    previousGainLinear = 0.0f;

    deviceManager.addAudioCallback (this);
    running.store (true, std::memory_order_relaxed);
    return true;
}

void AudioEngine::stop()
{
    if (! running.load (std::memory_order_relaxed))
        return;

    // Fade out before stopping the device so the transition never clicks.
    generator.startFade (false);

    int guardIterations = 0;
    while (generator.isFadeActive() && guardIterations++ < 400) // bounded: ~2 s max wait
        juce::Thread::sleep (5);

    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    running.store (false, std::memory_order_relaxed);
}

//==============================================================================
juce::String AudioEngine::getInputDeviceName() const
{
    const juce::ScopedLock sl (nameLock);
    return inputDeviceName;
}

juce::String AudioEngine::getOutputDeviceName() const
{
    const juce::ScopedLock sl (nameLock);
    return outputDeviceName;
}

//==============================================================================
int AudioEngine::readReferenceWaveform (float* dest, int maxSamples)
{
    return referenceHistory.readLatest (dest, 0, maxSamples, 0);
}

int AudioEngine::readInputWaveform (int channel, float* dest, int maxSamples)
{
    return inputHistory.readLatest (dest, channel, maxSamples, 0);
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    const double sampleRateNow = device->getCurrentSampleRate();
    const int    bufferSizeNow = device->getCurrentBufferSizeSamples();
    const int    numOutNow     = device->getActiveOutputChannels().countNumberOfSetBits();
    const int    numInNow      = device->getActiveInputChannels().countNumberOfSetBits();

    currentSampleRate.store (sampleRateNow, std::memory_order_relaxed);
    currentBufferSize.store (bufferSizeNow, std::memory_order_relaxed);
    currentNumOutputChannels.store (numOutNow, std::memory_order_relaxed);
    currentNumInputChannels.store (numInNow, std::memory_order_relaxed);

    {
        const juce::ScopedLock sl (nameLock);
        inputDeviceName  = device->getName();
        outputDeviceName = device->getName();
    }

    const int outCh = juce::jmax (1, numOutNow);
    const int inCh  = juce::jmax (1, numInNow);

    generator.prepare (sampleRateNow, bufferSizeNow);
    generator.setStereoMode (outCh >= 2 ? PinkNoiseGenerator::StereoMode::IndependentStereo
                                        : PinkNoiseGenerator::StereoMode::LinkedMono);

    referenceHistory.prepare (outCh, (int) (sampleRateNow * 2.0));
    inputHistory.prepare (inCh, (int) (sampleRateNow * 2.0));
    generator.setReferenceHistory (&referenceHistory);

    analysisProcessor.prepare (sampleRateNow, bufferSizeNow, inCh);
    eventDetector.prepare (sampleRateNow, bufferSizeNow);

    samplesSinceStart = 0;
    previousGainLinear = 0.0f;

    generator.reset();
    generator.startFade (true);
}

void AudioEngine::audioDeviceStopped()
{
    running.store (false, std::memory_order_relaxed);

    Event ev {};
    ev.type          = EventType::AudioDeviceLost;
    ev.severity      = Severity::Warning;
    ev.channel       = ChannelId::None;
    ev.elapsedSeconds = analysisProcessor.getElapsedSeconds();
    ev.code          = eventCodeInstant;

    eventQueue.push (ev);
}

void AudioEngine::audioDeviceError (const juce::String& errorMessage)
{
    juce::ignoreUnused (errorMessage);

    Event ev {};
    ev.type          = EventType::AudioDeviceLost;
    ev.severity      = Severity::Critical;
    ev.channel       = ChannelId::None;
    ev.elapsedSeconds = analysisProcessor.getElapsedSeconds();
    ev.code          = eventCodeInstant;

    eventQueue.push (ev);
}

//==============================================================================
void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                     int numInputChannels,
                                                     float* const* outputChannelData,
                                                     int numOutputChannels,
                                                     int numSamples,
                                                     const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused (context);

    // Defensive clear (generator overwrites every active channel, but any
    // channel beyond what the generator handles must still be silent).
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    // 1. Generate reference block -> output (writes ReferenceHistory itself).
    generator.processBlock (outputChannelData, numOutputChannels, numSamples);

    // Retain the returned input for the waveform display (no analysis use).
    if (numInputChannels > 0 && inputChannelData != nullptr)
        inputHistory.write (inputChannelData, numInputChannels, numSamples);

    // 2. Build statusBits.
    int32_t statusBits = StatusBits::ok;

    if (generator.isMuted())
        statusBits |= StatusBits::outputMuted;

    if (generator.isFadeActive())
    {
        const float gainNow = generator.getCurrentGainLinear();
        statusBits |= (gainNow >= previousGainLinear) ? StatusBits::fadingIn : StatusBits::fadingOut;
        previousGainLinear = gainNow;
    }
    else
    {
        previousGainLinear = generator.getCurrentGainLinear();
    }

    if (numInputChannels <= 0 || inputChannelData == nullptr)
        statusBits |= StatusBits::inputInactive;

    samplesSinceStart += numSamples;
    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr > 0.0)
    {
        const double elapsedSinceStart = (double) samplesSinceStart / sr;
        const double settlingSeconds = eventDetector.getThresholds().settlingMs / 1000.0;
        if (elapsedSinceStart < settlingSeconds)
            statusBits |= StatusBits::settling;
    }

    // 3. Analyse.
    const BlockStats stats = analysisProcessor.processBlock (inputChannelData, numInputChannels, numSamples,
                                                              referenceHistory, statusBits);

    // 4. Detect.
    const bool dropoutActive = eventDetector.processBlockStats (stats);
    if (dropoutActive)
        analysisProcessor.noteDropoutActive();
}

} // namespace pnam
