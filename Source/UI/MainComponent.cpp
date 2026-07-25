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
    statusBar.setSampleRate (engine->getSampleRate());
    statusBar.setBufferSize (engine->getBufferSize());
    statusBar.setChannelConfig (engine->getNumOutputChannels(), engine->getNumInputChannels());
    statusBar.setOutputDeviceName (engine->getOutputDeviceName());
    statusBar.setInputDeviceName (engine->getInputDeviceName());
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

    transport.onOpenSettings    = [this] { openSettings(); };
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

    auto* selector = new juce::AudioDeviceSelectorComponent (engine->getDeviceManager(),
                                                              0, 2,     // input channel range
                                                              0, 2,     // output channel range
                                                              false,    // no MIDI input selector
                                                              false,    // no MIDI output selector
                                                              true,     // show channels as stereo pairs
                                                              false);   // don't hide advanced options
    selector->setSize (500, 450);

    juce::DialogWindow::LaunchOptions launch;
    launch.content.setOwned (selector);
    launch.dialogTitle = "Audio Device Setup";
    launch.dialogBackgroundColour = Palette::get (Palette::panelBackground);
    launch.escapeKeyTriggersCloseButton = true;
    launch.useNativeTitleBar = true;
    launch.resizable = true;

    deviceSetupWindow = launch.launchAsync();
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
SessionInfo MainComponent::buildSessionInfo() const
{
    SessionInfo info;

    // sessionId is a fresh wall-clock label (unique per logging run, even
    // across multiple Start/Stop Logging cycles within one audio session);
    // startTimeUtc is the audio-clock zero anchor so elapsedSeconds -> UTC
    // conversion in SessionLogger stays correct regardless of when logging
    // was started relative to audio start.
    info.sessionId         = juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
    info.startTimeUtc      = sessionStartUtc;
    info.sampleRate        = engine->getSampleRate();
    info.bufferSize        = engine->getBufferSize();
    info.numOutputChannels = engine->getNumOutputChannels();
    info.numInputChannels  = engine->getNumInputChannels();
    info.inputDeviceName   = engine->getInputDeviceName();
    info.outputDeviceName  = engine->getOutputDeviceName();
    info.pinkNoiseSeed     = engine->getGenerator().getSeed();
    info.outputLevelDb     = engine->getGenerator().getLevelDb();

    return info;
}

juce::File MainComponent::getLogBaseFolder() const
{
    const auto stored = appProperties.getUserSettings()->getValue ("logBaseFolder", {});

    if (stored.isNotEmpty())
        return juce::File (stored);

    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
              .getChildFile ("PinkNoiseAudioMonitor");
}

} // namespace pnam
