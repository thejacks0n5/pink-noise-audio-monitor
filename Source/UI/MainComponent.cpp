/*
  ==============================================================================
    MainComponent.cpp
    Pink Noise Audio Monitor (pnam) — top-level component and message-thread hub.

    See MainComponent.h for the full ownership / threading / layout contract.
    This file wires every subsystem together: AudioEngine (audio thread glue),
    SessionLogger + NotificationEngine (message thread services), and all UI
    children, plus the single ~30 Hz juce::Timer that drains both FIFOs.
  ==============================================================================
*/

#include "MainComponent.h"
#include "../Core/AudioEngine.h"

namespace pnam
{

//==============================================================================
namespace
{
    /** Audio Device Setup dialog content: a juce::AudioDeviceSelectorComponent
        plus an explicit Done button.

        The selector applies every change to the AudioDeviceManager immediately
        (there is no "apply" step to forget), but users reasonably expect a way
        to confirm and dismiss, so Done closes the enclosing DialogWindow.

        Channels are shown individually rather than as stereo pairs, which is
        what makes mono input/output selectable. */
    class DeviceSetupPanel : public juce::Component
    {
    public:
        explicit DeviceSetupPanel (juce::AudioDeviceManager& dm)
            : selector (dm,
                        1, 2,      // input channel range  (min 1 => mono allowed)
                        1, 2,      // output channel range (min 1 => mono allowed)
                        false,     // no MIDI input selector
                        false,     // no MIDI output selector
                        false,     // show channels individually, NOT as stereo pairs
                        false)     // don't hide advanced options
        {
            addAndMakeVisible (selector);

            doneButton.onClick = [this]
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->closeButtonPressed();
            };

            addAndMakeVisible (doneButton);
            setSize (520, 520);
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced (8);
            auto footer = bounds.removeFromBottom (36);

            doneButton.setBounds (footer.removeFromRight (90).reduced (0, 4));
            selector.setBounds (bounds);
        }

    private:
        juce::AudioDeviceSelectorComponent selector;
        juce::TextButton doneButton { "Done" };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceSetupPanel)
    };

    /** Minimal modeless settings dialog content: EventDetector thresholds,
        NotificationEngine tuning, pink-noise seed (persisted; applied at the
        next Start Audio since PinkNoiseGenerator::setSeed() is non-real-time),
        and the log base folder. Deliberately self-contained so MainComponent
        doesn't need any new members beyond what MainComponent.h declares. */
    class SettingsPanel : public juce::Component
    {
    public:
        SettingsPanel (EventDetector& detectorIn, NotificationEngine& notifierIn,
                       juce::ApplicationProperties& propsIn)
            : detector (detectorIn), notifier (notifierIn), props (propsIn)
        {
            const auto thresholds = detector.getThresholds();

            setUpLabel (dropoutLabel, "Dropout threshold (dBFS)");
            setUpSlider (dropoutSlider, -80.0, -20.0, 0.5, thresholds.dropoutThresholdDb);
            dropoutSlider.onValueChange = [this] { applyThresholds(); };

            setUpLabel (levelTolLabel, "Level-change tolerance (dB)");
            setUpSlider (levelTolSlider, 1.0, 24.0, 0.5, thresholds.levelChangeToleranceDb);
            levelTolSlider.onValueChange = [this] { applyThresholds(); };

            setUpLabel (minDropoutLabel, "Min dropout hold (ms)");
            setUpSlider (minDropoutSlider, 10.0, 2000.0, 10.0, thresholds.minDropoutMs);
            minDropoutSlider.onValueChange = [this] { applyThresholds(); };

            setUpLabel (notifyToggleLabel, "Desktop notifications");
            addAndMakeVisible (notifyToggle);
            notifyToggle.setToggleState (notifier.isEnabled(), juce::dontSendNotification);
            notifyToggle.onClick = [this] { notifier.setEnabled (notifyToggle.getToggleState()); };

            setUpLabel (cooldownLabel, "Notification cooldown (s)");
            setUpSlider (cooldownSlider, 5.0, 600.0, 5.0, notifier.getCooldownSeconds());
            cooldownSlider.onValueChange = [this] { notifier.setCooldownSeconds (cooldownSlider.getValue()); };

            setUpLabel (seedLabel, "Pink noise seed (0 = auto, applies on next Start Audio)");
            addAndMakeVisible (seedEditor);
            seedEditor.setInputRestrictions (20, "0123456789");
            seedEditor.setText (props.getUserSettings()->getValue ("pinkNoiseSeed", "0"), juce::dontSendNotification);
            seedEditor.onFocusLost = [this] { props.getUserSettings()->setValue ("pinkNoiseSeed", seedEditor.getText()); };
            seedEditor.onReturnKey = [this] { props.getUserSettings()->setValue ("pinkNoiseSeed", seedEditor.getText()); };

            addAndMakeVisible (recaptureButton);
            recaptureButton.onClick = [this] { detector.recaptureBaseline(); };

            addAndMakeVisible (chooseFolderButton);
            chooseFolderButton.onClick = [this] { chooseLogFolder(); };

            setSize (420, 340);
        }

    private:
        void setUpLabel (juce::Label& label, const juce::String& text)
        {
            addAndMakeVisible (label);
            label.setText (text, juce::dontSendNotification);
            label.setFont (juce::Font (juce::FontOptions (13.0f)));
        }

        void setUpSlider (juce::Slider& slider, double lo, double hi, double step, double value)
        {
            addAndMakeVisible (slider);
            slider.setSliderStyle (juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
            slider.setRange (lo, hi, step);
            slider.setValue (value, juce::dontSendNotification);
        }

        void applyThresholds()
        {
            auto t = detector.getThresholds();
            t.dropoutThresholdDb     = (float) dropoutSlider.getValue();
            t.levelChangeToleranceDb = (float) levelTolSlider.getValue();
            t.minDropoutMs           = minDropoutSlider.getValue();
            detector.setThresholds (t);
        }

        void chooseLogFolder()
        {
            fileChooser = std::make_unique<juce::FileChooser> (
                "Choose log folder", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));

            fileChooser->launchAsync (
                juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                [this] (const juce::FileChooser& fc)
                {
                    auto result = fc.getResult();
                    if (result != juce::File())
                        props.getUserSettings()->setValue ("logBaseFolder", result.getFullPathName());
                });
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (12);

            auto row = [&b] (juce::Component& label, juce::Component& control, int h = 24)
            {
                auto r = b.removeFromTop (h);
                label.setBounds (r.removeFromLeft (230));
                control.setBounds (r);
                b.removeFromTop (6);
            };

            row (dropoutLabel, dropoutSlider);
            row (levelTolLabel, levelTolSlider);
            row (minDropoutLabel, minDropoutSlider);
            row (notifyToggleLabel, notifyToggle);
            row (cooldownLabel, cooldownSlider);

            auto seedRow = b.removeFromTop (40);
            seedLabel.setBounds (seedRow.removeFromTop (18));
            seedEditor.setBounds (seedRow);
            b.removeFromTop (10);

            recaptureButton.setBounds (b.removeFromTop (28));
            b.removeFromTop (6);
            chooseFolderButton.setBounds (b.removeFromTop (28));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::get (Palette::panelBackground));
        }

        EventDetector& detector;
        NotificationEngine& notifier;
        juce::ApplicationProperties& props;

        juce::Label dropoutLabel, levelTolLabel, minDropoutLabel, notifyToggleLabel, cooldownLabel, seedLabel;
        juce::Slider dropoutSlider, levelTolSlider, minDropoutSlider, cooldownSlider;
        juce::ToggleButton notifyToggle;
        juce::TextEditor seedEditor;
        juce::TextButton recaptureButton  { "Recapture Level Baseline" };
        juce::TextButton chooseFolderButton { "Choose Log Folder..." };
        std::unique_ptr<juce::FileChooser> fileChooser;
    };
}

//==============================================================================
MainComponent::MainComponent()
    : engine (std::make_unique<AudioEngine> (measurementQueue, eventQueue))
{
    juce::PropertiesFile::Options options;
    options.applicationName     = "PinkNoiseAudioMonitor";
    options.filenameSuffix      = "settings";
    options.folderName          = "PinkNoiseAudioMonitor";
    options.osxLibrarySubFolder = "Application Support";
    appProperties.setStorageParameters (options);

    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    // Preallocated once; reused every timer tick by updateMetersAndWaveform().
    waveformScratch.resize (8192, 0.0f);

    sessionLogger.onLoggingError = [this] (const juce::String& message)
    {
        statusBar.setLoggingState (TopStatusBar::LoggingState::writeError);

        Event e {};
        e.type           = EventType::LoggingError;
        e.severity       = Severity::Critical;
        e.channel        = ChannelId::None;
        e.elapsedSeconds = engine->getAnalysis().getElapsedSeconds();
        e.code           = eventCodeInstant;

        const auto description = SessionLogger::composeDescription (e) + ": " + message;
        eventLog.addEvent (e, description, sessionStartUtc + juce::RelativeTime (e.elapsedSeconds));
        maybeNotify (e, description);
    };

    notificationEngine.initialise();

    // Open the device up front (restoring the user's last selection) so Audio
    // Device Setup edits a live device before Start Audio is ever pressed, and
    // so the status bar shows the real interface immediately.
    {
        const auto savedState = appProperties.getUserSettings()->getXmlValue ("audioDeviceState");
        engine->initialiseDevices (savedState.get());
    }

    // Baseline for change reporting, captured before we start listening so the
    // initial device selection isn't reported as a "change".
    lastKnownSetup = engine->getDeviceManager().getAudioDeviceSetup();

    // Persist any device change the moment it happens, wherever it came from.
    engine->getDeviceManager().addChangeListener (this);

    addAndMakeVisible (statusBar);
    addAndMakeVisible (transport);
    addAndMakeVisible (refMeter);
    addAndMakeVisible (inputMeter);
    addAndMakeVisible (waveform);
    addAndMakeVisible (eventLog);

    refMeter.setSource (&engine->getAnalysis());
    inputMeter.setSource (&engine->getAnalysis());

    refMeter.onResetRequest   = [this] { engine->getAnalysis().resetPeakHold(); };
    inputMeter.onResetRequest = [this] { engine->getAnalysis().resetPeakHold(); };

    waveform.setNumInputChannels (2);   // best-guess default; corrected in startAudio()

    statusBar.onCellClicked = [this] (TopStatusBar::CellId id, juce::Rectangle<int> cellBounds)
    {
        showStatusBarDeviceMenu (id, cellBounds);
    };

    wireTransport();
    transport.setOutputLevelDb (PinkNoiseGenerator::defaultLevelDb);

    statusBar.setTestMode ("Pink noise loopback");
    statusBar.setAppStatus ("Idle");

    setSize (1120, 720);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();

    engine->getDeviceManager().removeChangeListener (this);
    saveDeviceState();

    if (settingsWindow != nullptr)
        settingsWindow.deleteAndZero();

    if (deviceSetupWindow != nullptr)
        deviceSetupWindow.deleteAndZero();

    // Stops logging (if active) and the audio engine (fades out first).
    stopAudio();

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::get (Palette::windowBackground));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (8);

    if (area.isEmpty())
        return;

    statusBar.setBounds (area.removeFromTop (TopStatusBar::preferredHeight));
    area.removeFromTop (8);

    transport.setBounds (area.removeFromTop (TransportControls::preferredHeight));
    area.removeFromTop (8);

    const int eventLogHeight = juce::jlimit (140, 280,
                                             (int) juce::roundToInt ((float) area.getHeight() * 0.30f));
    auto eventLogArea = area.removeFromBottom (eventLogHeight);
    area.removeFromBottom (8);
    eventLog.setBounds (eventLogArea);

    auto meterColumn = area.removeFromLeft (200);
    area.removeFromLeft (8);
    waveform.setBounds (area);

    auto refBounds = meterColumn.removeFromLeft (meterColumn.getWidth() / 2);
    meterColumn.removeFromLeft (4);
    refMeter.setBounds (refBounds);
    inputMeter.setBounds (meterColumn);
}

//==============================================================================
void MainComponent::timerCallback()
{
    // While the Audio Device Setup dialog is open, every device stop/restart is
    // the user changing something — don't report those as faults. (Changes made
    // from the status-bar menus are bracketed by applyDeviceSetup instead.)
    engine->setExpectedDeviceChange (deviceSetupWindow != nullptr);

    drainMeasurementQueue();
    drainEventQueue();
    updateMetersAndWaveform();
    updateStatusBar();
}

void MainComponent::drainMeasurementQueue()
{
    measurementQueue.popAll ([this] (const Measurement& m)
    {
        if (m.clipCountBlock > 0)
        {
            // Measurement carries an aggregate clip count only (no per-channel
            // breakdown), so light both channel indicators — the user clears
            // them via the meter's clip latch.
            inputMeter.notifyClipped (0);
            inputMeter.notifyClipped (1);
        }

        if (sessionLogger.isSessionActive() && ! loggingPaused)
            sessionLogger.logMeasurement (m);
    });
}

void MainComponent::drainEventQueue()
{
    eventQueue.popAll ([this] (const Event& e)
    {
        const auto description = SessionLogger::composeDescription (e);
        const auto wallClockUtc = sessionStartUtc + juce::RelativeTime (e.elapsedSeconds);

        eventLog.addEvent (e, description, wallClockUtc);

        if (e.type == EventType::Clipping && e.code == eventCodeStart)
        {
            if (e.channel == ChannelId::Left || e.channel == ChannelId::Both)
                inputMeter.notifyClipped (0);
            if (e.channel == ChannelId::Right || e.channel == ChannelId::Both)
                inputMeter.notifyClipped (1);
        }

        if (sessionLogger.isSessionActive() && ! loggingPaused)
            sessionLogger.logEvent (e, description);

        if (e.severity == Severity::Critical)
            maybeNotify (e, description);
    });
}

void MainComponent::updateMetersAndWaveform()
{
    refMeter.refresh();
    inputMeter.refresh();

    const double sr = engine->getSampleRate();

    if (sr > 0.0)
    {
        const int samplesToRead = juce::jlimit (1, (int) waveformScratch.size(),
                                                (int) juce::roundToInt (sr / 30.0));

        engine->readReferenceWaveform (waveformScratch.data(), samplesToRead);
        waveform.pushSamples (WaveformDisplay::Trace::reference, waveformScratch.data(), samplesToRead);

        engine->readInputWaveform (0, waveformScratch.data(), samplesToRead);
        waveform.pushSamples (WaveformDisplay::Trace::inputL, waveformScratch.data(), samplesToRead);

        if (engine->getNumInputChannels() >= 2)
        {
            engine->readInputWaveform (1, waveformScratch.data(), samplesToRead);
            waveform.pushSamples (WaveformDisplay::Trace::inputR, waveformScratch.data(), samplesToRead);
        }
    }

    waveform.refresh();
}

void MainComponent::updateStatusBar()
{
    statusBar.setSessionDuration (engine->isRunning() ? engine->getAnalysis().getElapsedSeconds() : -1.0);

    // Device identity comes from the AudioDeviceManager rather than the engine's
    // audio-thread atomics for two reasons: it is populated as soon as a device
    // is selected (the atomics only fill in once the callback is running, so the
    // bar read "—" until Start Audio), and it distinguishes the input from the
    // output device, where AudioIODevice::getName() reports one combined name.
    auto& deviceManager = engine->getDeviceManager();
    const auto setup    = deviceManager.getAudioDeviceSetup();

    statusBar.setOutputDeviceName (setup.outputDeviceName);
    statusBar.setInputDeviceName  (setup.inputDeviceName);

    if (engine->isRunning())
    {
        statusBar.setSampleRate (engine->getSampleRate());
        statusBar.setBufferSize (engine->getBufferSize());
        statusBar.setChannelConfig (engine->getNumOutputChannels(), engine->getNumInputChannels());
    }
    else if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        statusBar.setSampleRate (device->getCurrentSampleRate());
        statusBar.setBufferSize (device->getCurrentBufferSizeSamples());
        statusBar.setChannelConfig (device->getActiveOutputChannels().countNumberOfSetBits(),
                                    device->getActiveInputChannels().countNumberOfSetBits());
    }
    else
    {
        statusBar.setSampleRate (0.0);
        statusBar.setBufferSize (0);
        statusBar.setChannelConfig (0, 0);
    }
}

void MainComponent::maybeNotify (const Event& e, const juce::String& description)
{
    if (e.severity != Severity::Critical)
        return;

    lastNotifiedCritical = (float) e.elapsedSeconds;

    const juce::String title = juce::String ("Pink Noise Monitor: ") + toString (e.type);
    notificationEngine.postCritical (title, description, e.type);
}

//==============================================================================
void MainComponent::wireTransport()
{
    transport.onStartAudio  = [this] { startAudio(); };
    transport.onStopAudio   = [this] { stopAudio(); };

    transport.onStartLogging = [this] { startLogging(); };
    transport.onStopLogging  = [this] { stopLogging(); };
    transport.onPauseLogging = [this] (bool pause) { setLoggingPaused (pause); };

    transport.onMuteChanged        = [this] (bool mute) { setOutputMuted (mute); };
    transport.onOutputLevelChanged = [this] (double db) { setOutputLevelDb (db); };

    transport.onResetSession  = [this] { resetSession(); };
    transport.onAddMarker     = [this] { addManualMarker(); };
    transport.onExportSession = [this] { exportSession(); };

    transport.onOpenDeviceSetup = [this] { openAudioDeviceSetup(); };
}

//==============================================================================
void MainComponent::startAudio()
{
    if (engine->isRunning())
        return;

    statusBar.setAudioEngineState (TopStatusBar::EngineState::starting);

    // Apply a user-chosen seed (persisted via Settings) before starting;
    // PinkNoiseGenerator::setSeed() is non-real-time and must be called while
    // audio is stopped, which is guaranteed here.
    const juce::String seedText = appProperties.getUserSettings()->getValue ("pinkNoiseSeed", "0");
    const uint64_t seed = (uint64_t) seedText.getLargeIntValue();
    if (seed != 0)
        engine->getGenerator().setSeed (seed);

    if (engine->start())
    {
        sessionStartUtc = juce::Time::getCurrentTime();

        const int numIn = juce::jlimit (1, 2, engine->getNumInputChannels() > 0 ? engine->getNumInputChannels() : 1);
        inputMeter.setNumChannels (numIn);
        waveform.setSampleRate (engine->getSampleRate());
        waveform.setNumInputChannels (engine->getNumInputChannels());

        transport.setAudioRunning (true);
        statusBar.setAudioEngineState (TopStatusBar::EngineState::running);
        statusBar.setOutputDeviceName (engine->getOutputDeviceName());
        statusBar.setInputDeviceName (engine->getInputDeviceName());
        statusBar.setChannelConfig (engine->getNumOutputChannels(), engine->getNumInputChannels());
        statusBar.setSampleRate (engine->getSampleRate());
        statusBar.setBufferSize (engine->getBufferSize());
        statusBar.setAppStatus ("Monitoring");
    }
    else
    {
        transport.setAudioRunning (false);
        statusBar.setAudioEngineState (TopStatusBar::EngineState::deviceError);
        statusBar.setAppStatus ("Failed to start audio device");
    }
}

void MainComponent::stopAudio()
{
    if (sessionLogger.isSessionActive())
        stopLogging();

    engine->stop();

    transport.setAudioRunning (false);
    statusBar.setAudioEngineState (TopStatusBar::EngineState::stopped);
    statusBar.setAppStatus ("Idle");
}

void MainComponent::startLogging()
{
    if (! engine->isRunning())
        return;

    // A log name + folder is required. If it has never been set, prompt for it
    // and continue into logging once the user has chosen.
    if (! isLogDestinationConfigured())
    {
        chooseLogDestination ([this] (bool chosen)
        {
            if (chosen)
                startLogging();
        });

        return;
    }

    currentSessionInfo = buildSessionInfo();

    if (sessionLogger.startSession (getLogBaseFolder(), currentSessionInfo))
    {
        loggingPaused = false;
        transport.setLoggingActive (true);
        transport.setLoggingPaused (false);
        statusBar.setLoggingState (TopStatusBar::LoggingState::active);

        sessionAvailable = true;
        transport.setSessionAvailable (true);
        statusBar.setAppStatus ("Logging: " + currentSessionInfo.sessionId);
    }
    else
    {
        statusBar.setLoggingState (TopStatusBar::LoggingState::writeError);
        statusBar.setAppStatus ("Could not start logging session");
    }
}

void MainComponent::stopLogging()
{
    if (! sessionLogger.isSessionActive())
        return;

    sessionLogger.stopSession();
    currentSessionInfo = sessionLogger.getSessionInfo();

    loggingPaused = false;
    transport.setLoggingActive (false);
    transport.setLoggingPaused (false);
    statusBar.setLoggingState (TopStatusBar::LoggingState::inactive);

    sessionAvailable = true; // a completed session folder still exists to export
    transport.setSessionAvailable (true);
    statusBar.setAppStatus ("Idle");
}

void MainComponent::setLoggingPaused (bool shouldPause)
{
    if (! sessionLogger.isSessionActive())
        return;

    loggingPaused = shouldPause;
    transport.setLoggingPaused (loggingPaused);
    statusBar.setLoggingState (loggingPaused ? TopStatusBar::LoggingState::paused
                                              : TopStatusBar::LoggingState::active);
}

void MainComponent::setOutputLevelDb (double newLevelDb)
{
    engine->getGenerator().setLevelDb (newLevelDb);
}

void MainComponent::setOutputMuted (bool shouldMute)
{
    engine->getGenerator().setMuted (shouldMute);
    transport.setMuted (shouldMute);
}

void MainComponent::addManualMarker()
{
    Event e {};
    e.type           = EventType::ManualMarker;
    e.severity       = Severity::Info;
    e.channel        = ChannelId::None;
    e.elapsedSeconds = engine->getAnalysis().getElapsedSeconds();
    e.code           = eventCodeInstant;

    const auto description = SessionLogger::composeDescription (e);
    const auto wallClockUtc = sessionStartUtc + juce::RelativeTime (e.elapsedSeconds);

    // Injected directly (message thread), never through the audio-thread FIFO.
    eventLog.addEvent (e, description, wallClockUtc);

    if (sessionLogger.isSessionActive() && ! loggingPaused)
        sessionLogger.logEvent (e, description);
}

void MainComponent::resetSession()
{
    if (sessionLogger.isSessionActive())
        return;

    eventLog.clear();
    waveform.clear();

    engine->getAnalysis().resetPeakHold();
    engine->getDetector().recaptureBaseline();

    refMeter.clearClipIndicator();
    inputMeter.clearClipIndicator();

    sessionAvailable = false;
    transport.setSessionAvailable (false);
    currentSessionInfo = SessionInfo();

    statusBar.setAppStatus ("Idle");
}

void MainComponent::exportSession()
{
    const auto folder = sessionLogger.isSessionActive() ? sessionLogger.getSessionFolder()
                                                          : currentSessionInfo.logFolder;

    if (! folder.exists())
    {
        statusBar.setAppStatus ("No session available to export");
        return;
    }

    folder.revealToUser();
}

void MainComponent::openAudioDeviceSetup()
{
    if (deviceSetupWindow != nullptr)
    {
        deviceSetupWindow->toFront (true);
        return;
    }

    juce::DialogWindow::LaunchOptions launch;
    launch.content.setOwned (new DeviceSetupPanel (engine->getDeviceManager()));
    launch.dialogTitle = "Audio Device Setup";
    launch.dialogBackgroundColour = Palette::get (Palette::panelBackground);
    launch.escapeKeyTriggersCloseButton = true;
    launch.useNativeTitleBar = true;
    launch.resizable = true;

    deviceSetupWindow = launch.launchAsync();

    // Take effect immediately rather than on the next timer tick, in case the
    // user changes a device before then.
    engine->setExpectedDeviceChange (true);
}

void MainComponent::openSettings()
{
    if (settingsWindow != nullptr)
    {
        settingsWindow->toFront (true);
        return;
    }

    auto* content = new SettingsPanel (engine->getDetector(), notificationEngine, appProperties);

    juce::DialogWindow::LaunchOptions launch;
    launch.content.setOwned (content);
    launch.dialogTitle = "Settings";
    launch.dialogBackgroundColour = Palette::get (Palette::panelBackground);
    launch.escapeKeyTriggersCloseButton = true;
    launch.useNativeTitleBar = true;
    launch.resizable = false;

    settingsWindow = launch.launchAsync();
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source != &engine->getDeviceManager())
        return;

    logAudioSettingsChange();
    saveDeviceState();
}

void MainComponent::logAudioSettingsChange()
{
    const auto current = engine->getDeviceManager().getAudioDeviceSetup();

    const auto channelsOf = [] (const juce::AudioDeviceManager::AudioDeviceSetup& s)
    {
        return juce::String (s.outputChannels.countNumberOfSetBits()) + " out / "
             + juce::String (s.inputChannels.countNumberOfSetBits()) + " in";
    };

    juce::StringArray changes;

    if (current.outputDeviceName != lastKnownSetup.outputDeviceName)
        changes.add ("output \"" + lastKnownSetup.outputDeviceName + "\" -> \"" + current.outputDeviceName + "\"");

    if (current.inputDeviceName != lastKnownSetup.inputDeviceName)
        changes.add ("input \"" + lastKnownSetup.inputDeviceName + "\" -> \"" + current.inputDeviceName + "\"");

    if (std::abs (current.sampleRate - lastKnownSetup.sampleRate) > 0.5)
        changes.add ("sample rate " + juce::String (lastKnownSetup.sampleRate, 0)
                        + " -> " + juce::String (current.sampleRate, 0) + " Hz");

    if (current.bufferSize != lastKnownSetup.bufferSize)
        changes.add ("buffer " + juce::String (lastKnownSetup.bufferSize)
                        + " -> " + juce::String (current.bufferSize) + " smp");

    if (channelsOf (current) != channelsOf (lastKnownSetup))
        changes.add ("channels " + channelsOf (lastKnownSetup) + " -> " + channelsOf (current));

    lastKnownSetup = current;

    // The manager broadcasts for reasons other than a setup change too; only log
    // when something a reader would care about actually moved.
    if (changes.isEmpty())
        return;

    Event e {};
    e.type           = EventType::AudioSettingsChanged;
    e.severity       = Severity::Info;
    e.channel        = ChannelId::None;
    e.elapsedSeconds = engine->getAnalysis().getElapsedSeconds();
    e.code           = eventCodeInstant;

    const auto description = "Audio settings changed: " + changes.joinIntoString (", ");
    const auto wallClockUtc = sessionStartUtc + juce::RelativeTime (e.elapsedSeconds);

    // Message-thread injection, same path as a manual marker.
    eventLog.addEvent (e, description, wallClockUtc);

    if (sessionLogger.isSessionActive() && ! loggingPaused)
        sessionLogger.logEvent (e, description);
}

juce::String MainComponent::applyDeviceSetup (const juce::AudioDeviceManager::AudioDeviceSetup& newSetup)
{
    // The device stops and restarts inside setAudioDeviceSetup(); bracket it so
    // that transition is not detected/logged as an audio fault.
    engine->setExpectedDeviceChange (true);
    const auto error = engine->getDeviceManager().setAudioDeviceSetup (newSetup, true);
    engine->setExpectedDeviceChange (deviceSetupWindow != nullptr);

    return error;
}

void MainComponent::saveDeviceState()
{
    if (auto stateXml = engine->createDeviceStateXml())
        appProperties.getUserSettings()->setValue ("audioDeviceState", stateXml.get());
    else
        appProperties.getUserSettings()->removeValue ("audioDeviceState");

    appProperties.getUserSettings()->saveIfNeeded();
}

void MainComponent::showStatusBarDeviceMenu (TopStatusBar::CellId cellId,
                                             juce::Rectangle<int> cellScreenBounds)
{
    auto& deviceManager = engine->getDeviceManager();
    const auto setup    = deviceManager.getAudioDeviceSetup();
    auto* currentDevice = deviceManager.getCurrentAudioDevice();

    constexpr int openFullSetupId = 10000;

    juce::PopupMenu menu;
    juce::StringArray deviceNames;   // populated for the two device cells
    juce::Array<double> sampleRates;
    juce::Array<int> bufferSizes;

    switch (cellId)
    {
        case TopStatusBar::CellId::outDevice:
        case TopStatusBar::CellId::inDevice:
        {
            const bool wantInputs = (cellId == TopStatusBar::CellId::inDevice);

            if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            {
                type->scanForDevices();
                deviceNames = type->getDeviceNames (wantInputs);
            }

            const auto& active = wantInputs ? setup.inputDeviceName : setup.outputDeviceName;

            for (int i = 0; i < deviceNames.size(); ++i)
                menu.addItem (i + 1, deviceNames[i], true, deviceNames[i] == active);

            if (deviceNames.isEmpty())
                menu.addItem (-1, "No devices found", false, false);

            break;
        }

        case TopStatusBar::CellId::channels:
        {
            const int activeChannels = juce::jmax (setup.inputChannels.countNumberOfSetBits(),
                                                   setup.outputChannels.countNumberOfSetBits());

            menu.addItem (1, "Mono (1 in / 1 out)",   true, activeChannels == 1);
            menu.addItem (2, "Stereo (2 in / 2 out)", true, activeChannels >= 2);
            break;
        }

        case TopStatusBar::CellId::rate:
        {
            if (currentDevice != nullptr)
            {
                sampleRates = currentDevice->getAvailableSampleRates();

                for (int i = 0; i < sampleRates.size(); ++i)
                {
                    const auto rate = sampleRates[i];
                    menu.addItem (i + 1,
                                  juce::String (rate / 1000.0, rate < 100000.0 ? 1 : 0) + " kHz",
                                  true,
                                  std::abs (rate - setup.sampleRate) < 1.0);
                }
            }

            if (sampleRates.isEmpty())
                menu.addItem (-1, "Start audio to see rates", false, false);

            break;
        }

        case TopStatusBar::CellId::buffer:
        {
            if (currentDevice != nullptr)
            {
                bufferSizes = currentDevice->getAvailableBufferSizes();

                for (int i = 0; i < bufferSizes.size(); ++i)
                {
                    const auto size = bufferSizes[i];
                    const auto ms   = setup.sampleRate > 0.0
                                        ? " (" + juce::String (1000.0 * size / setup.sampleRate, 1) + " ms)"
                                        : juce::String();

                    menu.addItem (i + 1, juce::String (size) + " smp" + ms,
                                  true, size == setup.bufferSize);
                }
            }

            if (bufferSizes.isEmpty())
                menu.addItem (-1, "Start audio to see buffer sizes", false, false);

            break;
        }

        case TopStatusBar::CellId::none:
        case TopStatusBar::CellId::audio:
        case TopStatusBar::CellId::logging:
        case TopStatusBar::CellId::duration:
        case TopStatusBar::CellId::mode:
        case TopStatusBar::CellId::appStatus:
            return;   // non-interactive cell
    }

    menu.addSeparator();
    menu.addItem (openFullSetupId, "Audio Device Setup...");

    juce::Component::SafePointer<MainComponent> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (cellScreenBounds),
                        [safeThis, cellId, deviceNames, sampleRates, bufferSizes] (int result)
    {
        if (safeThis == nullptr || result <= 0)
            return;

        if (result == openFullSetupId)
        {
            safeThis->openAudioDeviceSetup();
            return;
        }

        auto& dm = safeThis->engine->getDeviceManager();
        auto newSetup = dm.getAudioDeviceSetup();

        switch (cellId)
        {
            case TopStatusBar::CellId::outDevice:
                newSetup.outputDeviceName = deviceNames[result - 1];
                break;

            case TopStatusBar::CellId::inDevice:
                newSetup.inputDeviceName = deviceNames[result - 1];
                break;

            case TopStatusBar::CellId::channels:
            {
                juce::BigInteger channels;
                channels.setRange (0, result == 1 ? 1 : 2, true);

                newSetup.inputChannels             = channels;
                newSetup.outputChannels            = channels;
                newSetup.useDefaultInputChannels   = false;
                newSetup.useDefaultOutputChannels  = false;
                break;
            }

            case TopStatusBar::CellId::rate:
                newSetup.sampleRate = sampleRates[result - 1];
                break;

            case TopStatusBar::CellId::buffer:
                newSetup.bufferSize = bufferSizes[result - 1];
                break;

            case TopStatusBar::CellId::none:
            case TopStatusBar::CellId::audio:
            case TopStatusBar::CellId::logging:
            case TopStatusBar::CellId::duration:
            case TopStatusBar::CellId::mode:
            case TopStatusBar::CellId::appStatus:
                return;
        }

        const auto error = safeThis->applyDeviceSetup (newSetup);

        if (error.isNotEmpty())
            safeThis->statusBar.setAppStatus ("Device error: " + error);
        else
            safeThis->saveDeviceState();   // also fires via changeListenerCallback
    });
}

//==============================================================================
SessionInfo MainComponent::buildSessionInfo() const
{
    SessionInfo info;

    // sessionId is a fresh wall-clock label (unique per logging run, even
    // across multiple Start/Stop Logging cycles within one audio session);
    // startTimeUtc is the audio-clock zero anchor so elapsedSeconds -> UTC
    // conversion in SessionLogger stays correct regardless of when logging
    // was started relative to audio start.
    // "<user's log name>_<timestamp>": carries the name the user chose while
    // still keeping every run in its own folder.
    const auto timestamp = juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
    const auto baseName  = getLogBaseName();

    info.sessionId         = baseName.isNotEmpty() ? baseName + "_" + timestamp : timestamp;
    info.startTimeUtc      = sessionStartUtc;
    info.sampleRate        = engine->getSampleRate();
    info.bufferSize        = engine->getBufferSize();
    info.numOutputChannels = engine->getNumOutputChannels();
    info.numInputChannels  = engine->getNumInputChannels();
    // From the device manager, so session_info.txt records the distinct input
    // and output devices rather than one combined AudioIODevice name.
    const auto deviceSetup = engine->getDeviceManager().getAudioDeviceSetup();
    info.inputDeviceName   = deviceSetup.inputDeviceName;
    info.outputDeviceName  = deviceSetup.outputDeviceName;
    info.pinkNoiseSeed     = engine->getGenerator().getSeed();
    info.outputLevelDb     = engine->getGenerator().getLevelDb();

    return info;
}

void MainComponent::openLogFileSetup()
{
    chooseLogDestination (nullptr);
}

juce::String MainComponent::getLogBaseName() const
{
    auto& props = const_cast<juce::ApplicationProperties&> (appProperties);
    return props.getUserSettings()->getValue ("logBaseName", {});
}

bool MainComponent::isLogDestinationConfigured() const
{
    auto& props = const_cast<juce::ApplicationProperties&> (appProperties);

    return getLogBaseName().isNotEmpty()
        && props.getUserSettings()->getValue ("logBaseFolder", {}).isNotEmpty();
}

void MainComponent::chooseLogDestination (std::function<void (bool)> onChosen)
{
    const auto existingName = getLogBaseName();
    const auto startIn = getLogBaseFolder().getChildFile (existingName.isNotEmpty() ? existingName
                                                                                    : "PinkNoiseLog");

    logFileChooser = std::make_unique<juce::FileChooser> (
        "Name this log and choose where to save it", startIn, "*");

    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    logFileChooser->launchAsync (flags, [this, onChosen] (const juce::FileChooser& chooser)
    {
        const auto result = chooser.getResult();

        if (result == juce::File{})
        {
            if (onChosen != nullptr)
                onChosen (false);

            return;
        }

        // The name becomes the label on each session folder; the containing
        // directory becomes the log root. Each Start Logging still gets its own
        // "<name>_<timestamp>" folder so runs never overwrite each other.
        auto* settings = appProperties.getUserSettings();
        settings->setValue ("logBaseName",   result.getFileNameWithoutExtension());
        settings->setValue ("logBaseFolder", result.getParentDirectory().getFullPathName());
        settings->saveIfNeeded();

        statusBar.setAppStatus ("Log: " + result.getFileNameWithoutExtension()
                                  + " in " + result.getParentDirectory().getFileName());

        if (onChosen != nullptr)
            onChosen (true);
    });
}

juce::File MainComponent::getLogBaseFolder() const
{
    // getUserSettings() is non-const in JUCE; this accessor is logically const.
    auto& props = const_cast<juce::ApplicationProperties&> (appProperties);
    const auto stored = props.getUserSettings()->getValue ("logBaseFolder", {});

    if (stored.isNotEmpty())
        return juce::File (stored);

    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
              .getChildFile ("PinkNoiseAudioMonitor");
}

} // namespace pnam
