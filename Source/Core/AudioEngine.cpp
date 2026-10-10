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
      eventDetector (eventQueueToUse),
      artifactDetector (eventQueueToUse)
{
    analysisProcessor.setArtifactDetector (&artifactDetector);
    acquisitionThread.startThread (juce::Thread::Priority::low);
}

AudioEngine::~AudioEngine()
{
    // Detach from the device FIRST. deviceManager is declared before the
    // generator, histories, analysis and detectors, so it is destroyed after
    // them — and until it is, the IO thread would go on calling into members
    // (the detectors' buffers, the analysis scratch) that no longer exist.
    // removeAudioCallback blocks until any callback in progress has returned;
    // the bracket keeps the resulting audioDeviceStopped from being reported as
    // a device loss.
    expectedDeviceChange.store (true, std::memory_order_relaxed);
    deviceManager.removeAudioCallback (this);

    stop();
    acquisitionThread.stopThread (2000);
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
    // A session is already open. Restarting here would zero the session clock
    // and wipe the detector underneath it — and underneath an open log, whose
    // elapsed_s would jump back to 0 mid-file. Only stop() ends a session.
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
    // The detach fires audioDeviceStopped, which must not be reported as a
    // device loss — without this bracket every Start Audio logged a spurious
    // AudioDeviceLost warning.
    const bool wasExpected = expectedDeviceChange.load (std::memory_order_relaxed);
    expectedDeviceChange.store (true, std::memory_order_relaxed);

    deviceManager.removeAudioCallback (this);

    // A genuine session start: clock to zero and every detector state machine
    // wiped. Without the detector reset a stopped-and-restarted session keeps a
    // rate-limit window set in the future and silently drops events until the
    // clock catches up — hours, after a long run.
    analysisProcessor.resetSessionClock();
    eventDetector.resetSessionState();
    artifactDetector.resetSessionState();

    samplesSinceStart  = 0;
    previousGainLinear = 0.0f;

    generator.reset();

    // Start Audio is the user explicitly asking for output on the device that
    // is open now, so it also releases any hold left by an earlier device loss.
    deviceLossSilenced.store (false, std::memory_order_release);

    // Release-store pairs with the acquire load in the callback, so the
    // generator's reset PRNG/filter state is visible before it is used.
    outputSilenced.store (false, std::memory_order_release);

    // Re-attaching calls audioDeviceAboutToStart, which prepares the generator,
    // histories, analysis and detector for the live device and starts the fade.
    deviceManager.addAudioCallback (this);
    expectedDeviceChange.store (wasExpected, std::memory_order_relaxed);

    running.store (true, std::memory_order_relaxed);
    return true;
}

void AudioEngine::stop()
{
    // Whatever state we are in, "stopped" means no noise at the output.
    if (! running.load (std::memory_order_relaxed))
    {
        outputSilenced.store (true, std::memory_order_release);
        return;
    }

    // Fade out before stopping the device so the transition never clicks. Only
    // worth waiting for if the generator is actually being run: with no device,
    // or with the output already held silent, the fade would never complete.
    if (deviceActive.load (std::memory_order_relaxed)
        && ! deviceLossSilenced.load (std::memory_order_relaxed))
    {
        generator.startFade (false);

        int guardIterations = 0;
        while (generator.isFadeActive() && guardIterations++ < 400) // bounded: ~2 s max wait
            juce::Thread::sleep (5);
    }

    // Keep the callback attached and the device open: output goes silent but
    // input is still captured, so the meters keep moving and the user can see
    // which input they are patching.
    outputSilenced.store (true, std::memory_order_release);

    running.store (false, std::memory_order_relaxed);
}

void AudioEngine::holdOutputForDeviceLoss() noexcept
{
    // Same hold an unexpected device stop sets. If one is already in force it
    // keeps the device it is waiting for.
    if (deviceLossSilenced.exchange (true, std::memory_order_acq_rel))
        return;

    lossWallMs.store (juce::Time::currentTimeMillis(), std::memory_order_relaxed);

    // The device to wait for is the one the user chose, which the device
    // manager still remembers: JUCE's fallback to a default device does not
    // overwrite the explicit selection. A combined in/out device is named after
    // its output (see CoreAudioIODeviceType::createDevice). With no explicit
    // selection on record there is nothing to wait for, and only
    // clearDeviceLossSilence() or a fresh start() releases the hold.
    juce::String chosen;

    if (const auto state = deviceManager.createStateXml())
    {
        chosen = state->getStringAttribute ("audioOutputDeviceName");

        if (chosen.isEmpty())
            chosen = state->getStringAttribute ("audioDeviceName");

        if (chosen.isEmpty())
            chosen = state->getStringAttribute ("audioInputDeviceName");
    }

    const juce::ScopedLock sl (nameLock);
    lostDeviceName = chosen;
}

void AudioEngine::clearDeviceLossSilence() noexcept
{
    if (! deviceLossSilenced.load (std::memory_order_relaxed))
        return;

    // Treat the resumption like a device start: fade in from silence (the hold
    // cut the output, it did not fade it — and after holdOutputForDeviceLoss()
    // there may have been no device restart to reset the fade), and restart the
    // settling window so the first seconds of returning signal are not judged.
    if (! generator.isMuted())
        generator.startFadeInFromSilence();

    settleRestartRequested.store (true, std::memory_order_release);
    deviceLossSilenced.store (false, std::memory_order_release);
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

    // If the device stopped on its own, is it the same one coming back?
    bool resumedAfterLoss = false;

    {
        const juce::ScopedLock sl (nameLock);
        inputDeviceName  = device->getName();
        outputDeviceName = device->getName();

        resumedAfterLoss = deviceLossSilenced.load (std::memory_order_relaxed)
                             && lostDeviceName.isNotEmpty()
                             && lostDeviceName == outputDeviceName;
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
    artifactDetector.prepare (sampleRateNow, bufferSizeNow, inCh, outCh);

    samplesSinceStart = 0;
    previousGainLinear = 0.0f;

    generator.reset();

    // Fade in — unless the user has Mute on. Mute is only a fade target inside
    // the generator, so an unconditional fade-in here silently un-muted the
    // output on every device (re)start while the Mute button stayed lit and
    // detection stayed suppressed.
    generator.startFade (! generator.isMuted());

    deviceActive.store (true, std::memory_order_relaxed);

    // The device that dropped out from under us is back (a hiccup, a clock
    // renegotiation, a replug that JUCE reopened): resume by ourselves, so an
    // unattended run recovers. Any OTHER device stays silent until the UI, which
    // knows what the user chose, calls clearDeviceLossSilence().
    if (resumedAfterLoss)
    {
        deviceLossSilenced.store (false, std::memory_order_release);

        // An INSTANT row of its own, not the "end" of the loss: the loss was
        // logged as an instant event, so an End here would be an end without a
        // start. Severity Info is what tells it apart from the loss itself
        // (SessionLogger::composeDescription words it accordingly);
        // measuredValue = the outage in seconds. Wall clock, not the
        // millisecond counter: that one stands still while the machine sleeps,
        // which is exactly when devices go away for hours.
        Event ev {};
        ev.type           = EventType::AudioDeviceLost;
        ev.severity       = Severity::Info;
        ev.channel        = ChannelId::None;
        ev.elapsedSeconds = analysisProcessor.getElapsedSeconds();
        ev.measuredValue  = (double) (juce::Time::currentTimeMillis()
                                        - lossWallMs.load (std::memory_order_relaxed)) / 1000.0;
        ev.code           = eventCodeInstant;

        eventQueue.push (ev);
    }
}

void AudioEngine::audioDeviceStopped()
{
    deviceActive.store (false, std::memory_order_relaxed);

    // A user-initiated reconfiguration stops and restarts the device. That is
    // not a fault: nothing to report, nothing to silence.
    if (expectedDeviceChange.load (std::memory_order_relaxed))
        return;

    // NOT the end of the session: `running` is left alone. JUCE restarts the
    // device (or opens another) by itself, the callback resumes, and clearing
    // the flag here left the app believing it was stopped while it was still
    // playing and logging — after which Start Audio zeroed the clock under the
    // open log and Stop Audio did nothing at all.
    //
    // What does change is the output: hold it silent from here on. JUCE's
    // fallback when the device has gone is the system default device, and noise
    // must not come out of that. First loss only — a second stop while already
    // held (the fallback device going away too) must not overwrite which device
    // we are waiting for. May run off the message thread; nothing here is
    // audio-thread state.
    if (! deviceLossSilenced.exchange (true, std::memory_order_acq_rel))
    {
        lossWallMs.store (juce::Time::currentTimeMillis(), std::memory_order_relaxed);

        const juce::ScopedLock sl (nameLock);
        lostDeviceName = outputDeviceName;
    }

    // Critical: an unattended run has just stopped testing anything, which is
    // exactly what the desktop notification is for.
    Event ev {};
    ev.type          = EventType::AudioDeviceLost;
    ev.severity      = Severity::Critical;
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
    const bool silenced = outputSilenced.load (std::memory_order_acquire)
                            || deviceLossSilenced.load (std::memory_order_acquire);

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

    if (settleRestartRequested.exchange (false, std::memory_order_acq_rel))
        samplesSinceStart = 0;

    samplesSinceStart += numSamples;
    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr > 0.0)
    {
        const double elapsedSinceStart = (double) samplesSinceStart / sr;
        // Wait-free atomic, not getThresholds(): that is a seqlock read which
        // retries until stable and could spin here for as long as the message
        // thread stays preempted mid-write.
        const double settlingSeconds = eventDetector.getSettlingMs() / 1000.0;
        if (elapsedSinceStart < settlingSeconds)
            statusBits |= StatusBits::settling;
    }

    // 3. Analyse.
    // 3a. Artifacts: the return against the exact block just sent. This runs
    //     first so the measurement row the analysis stage may emit below can
    //     carry this block's figures. The channel mapping lives in the analysis
    //     stage (the UI sets it there); mirror it rather than keep two copies
    //     in step from the message thread.
    for (int ch = 0; ch < juce::jmin (numInputChannels, maxChannels); ++ch)
    {
        artifactDetector.setReferenceOutputForInput (ch, analysisProcessor.getReferenceOutputForInput (ch));
        artifactDetector.setInputMonitored (ch, analysisProcessor.isInputMonitored (ch));
    }

    artifactDetector.processBlock (inputChannelData, numInputChannels, outputChannelData, numOutputChannels,
                                   numSamples, statusBits, analysisProcessor.getElapsedSeconds());

    const BlockStats stats = analysisProcessor.processBlock (inputChannelData, numInputChannels, numSamples,
                                                              referenceHistory, statusBits);

    // 4. Detect.
    const bool dropoutActive = eventDetector.processBlockStats (stats);
    if (dropoutActive)
        analysisProcessor.noteDropoutActive();
}

} // namespace pnam
