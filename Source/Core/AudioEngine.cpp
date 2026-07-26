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
juce::String AudioEngine::initialiseDevices (const juce::XmlElement* savedState)
{
    // Restore the user's saved selection when we have one; fall back to system
    // defaults only if that fails (selectDefaultDeviceOnFailure = true).
    juce::String error = deviceManager.initialise (2, 2, savedState, true);

    if (error.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr)
        error = deviceManager.initialise (1, 1, savedState, true);

    devicesInitialised = deviceManager.getCurrentAudioDevice() != nullptr;

    // Attach the callback immediately, with output silenced. Input is captured
    // and analysed from this point, so the input meters move while the user is
    // patching — before Start Audio has ever been pressed.
    if (devicesInitialised)
    {
        outputSilenced.store (true, std::memory_order_release);
        deviceManager.addAudioCallback (this);
    }

    return error;
}

std::unique_ptr<juce::XmlElement> AudioEngine::createDeviceStateXml() const
{
    return deviceManager.createStateXml();
}

bool AudioEngine::start()
{
    if (running.load (std::memory_order_relaxed))
        return true;

    // Only pick devices if nobody ever configured them. Re-running initialise()
    // here would silently throw away the user's Audio Device Setup choice.
    if (! devicesInitialised)
        initialiseDevices (nullptr);

    // stop() closes the device but the manager remembers the setup, so this
    // reopens exactly what the user selected.
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        deviceManager.restartLastAudioDevice();

    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return false;

    // DETACH before touching audio-thread state. Since the callback is now
    // attached from launch (to keep input meters live), "silencing" does not
    // stop it — processBlock keeps running. Resetting the non-atomic session
    // clock underneath a live callback is a data race, and the reset can be
    // lost when the audio thread writes back a stale sample count.
    // removeAudioCallback blocks until the callback returns, so this is safe.
    deviceManager.removeAudioCallback (this);

    // A genuine session start: clock to zero and every detector state machine
    // wiped. Without the detector reset a stopped-and-restarted session keeps a
    // rate-limit window set in the future and silently drops events until the
    // clock catches up — hours, after a long run.
    analysisProcessor.resetSessionClock();
    eventDetector.resetSessionState();

    samplesSinceStart  = 0;
    previousGainLinear = 0.0f;

    generator.reset();

    // Release-store pairs with the acquire load in the callback, so the
    // generator's reset PRNG/filter state is visible before it is used.
    outputSilenced.store (false, std::memory_order_release);

    // Re-attaching calls audioDeviceAboutToStart, which prepares the generator,
    // histories, analysis and detector for the live device and starts the fade.
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

    // Keep the callback attached and the device open: output goes silent but
    // input is still captured, so the meters keep moving and the user can see
    // which input they are patching.
    outputSilenced.store (true, std::memory_order_release);

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
int AudioEngine::readReferenceWaveform (int channel, float* dest, int maxSamples)
{
    return referenceHistory.readLatest (dest, channel, maxSamples, 0);
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
    // Always independent: every output carries its own uniquely seeded stream
    // (seed, seed+1, ...), which is what lets an input be traced back to the
    // specific output feeding it and makes a swapped patch cable detectable.
    generator.setStereoMode (PinkNoiseGenerator::StereoMode::IndependentStereo);

    referenceHistory.prepare (outCh, (int) (sampleRateNow * 2.0));
    inputHistory.prepare (inCh, (int) (sampleRateNow * 2.0));
    generator.setReferenceHistory (&referenceHistory);

    analysisProcessor.prepare (sampleRateNow, bufferSizeNow, inCh, outCh);
    eventDetector.prepare (sampleRateNow, bufferSizeNow);

    samplesSinceStart = 0;
    previousGainLinear = 0.0f;

    generator.reset();
    generator.startFade (true);
}

void AudioEngine::audioDeviceStopped()
{
    // A user-initiated reconfiguration stops and restarts the device. That is
    // not a fault, and clearing `running` here would leave the session marked
    // stopped even though audio resumes a moment later.
    if (expectedDeviceChange.load (std::memory_order_relaxed))
        return;

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

    // 1. Reference block -> output. While silenced the outputs stay at the zero
    //    fill above, and that silence is written to the reference history so the
    //    reference meters read silence instead of holding a stale picture.
    const bool silenced = outputSilenced.load (std::memory_order_acquire);

    if (silenced)
        referenceHistory.write (outputChannelData, numOutputChannels, numSamples);
    else
        generator.processBlock (outputChannelData, numOutputChannels, numSamples);

    // Retain the returned input for the waveform display (no analysis use).
    if (numInputChannels > 0 && inputChannelData != nullptr)
        inputHistory.write (inputChannelData, numInputChannels, numSamples);

    // 2. Build statusBits.
    int32_t statusBits = StatusBits::ok;

    // Silenced output must suppress detection: with no reference signal every
    // input would otherwise look like a complete dropout.
    if (silenced || generator.isMuted())
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
