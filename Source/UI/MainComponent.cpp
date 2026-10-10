/*
  ==============================================================================
    MainComponent.cpp
    Pink Noise Audio Monitor (pnam) — top-level component and message-thread hub.

    See MainComponent.h for the full ownership / threading / layout contract.
    This file wires every subsystem together: AudioEngine (audio thread glue),
    SessionLogger + NotificationEngine (message thread services), and all UI
    children, plus the single ~30 Hz juce::Timer that drains both FIFOs (the
    event FIFO is also drained on demand by startAudio() and
    beginLoggingSession(); always on the message thread).
  ==============================================================================
*/

#include "MainComponent.h"
#include "../Core/AudioEngine.h"

#include <algorithm>

#if JUCE_MAC
 #include <IOKit/pwr_mgt/IOPMLib.h>   // sleep assertion held while logging
#endif

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
    class ChannelSetupPanel;   // defined below; embedded in the setup dialog

    class DeviceSetupPanel : public juce::Component,
                             private juce::ChangeListener
    {
    public:
        /** @param channelCountsNow  live { outputs, inputs } channel counts.
            @param makeNamingSection builds the channel-naming section for the
                                     current device; called again whenever the
                                     counts change while the dialog is open. */
        DeviceSetupPanel (juce::AudioDeviceManager& dm,
                          std::function<std::pair<int, int>()> channelCountsNow,
                          std::function<juce::Component*()> makeNamingSection)
            : deviceManager (dm),
              selector (dm,
                        1, maxChannels,   // input channel range  (min 1 => mono allowed)
                        1, maxChannels,   // output channel range (min 1 => mono allowed)
                        false,     // no MIDI input selector
                        false,     // no MIDI output selector
                        false,     // show channels individually, NOT as stereo pairs
                        false),    // don't hide advanced options
              getChannelCounts (std::move (channelCountsNow)),
              createNaming (std::move (makeNamingSection))
        {
            addAndMakeVisible (selector);

            // Channel naming lives here rather than in its own dialog: device
            // choice and channel identity are the same job.
            namingViewport.setScrollBarsShown (true, false);
            addAndMakeVisible (namingViewport);
            rebuildNaming();

            // The naming rows used to be built once, so picking the interface
            // and ticking its channels above left the old device's rows below
            // until the dialog was closed and reopened.
            deviceManager.addChangeListener (this);

            doneButton.onClick = [this]
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->closeButtonPressed();
            };

            addAndMakeVisible (doneButton);
            setSize (700, naming != nullptr ? 720 : 520);
        }

        ~DeviceSetupPanel() override
        {
            deviceManager.removeChangeListener (this);
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced (8);
            auto footer = bounds.removeFromBottom (36);

            doneButton.setBounds (footer.removeFromRight (90).reduced (0, 4));

            if (naming != nullptr)
            {
                // Split the dialog: devices on top, channel naming underneath.
                auto namingArea = bounds.removeFromBottom (juce::jmax (140, bounds.getHeight() / 2));
                bounds.removeFromBottom (8);

                namingViewport.setBounds (namingArea);
                naming->setSize (juce::jmax (namingArea.getWidth() - 16, 560),
                                 naming->getHeight());
            }

            selector.setBounds (bounds);
        }

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            // The manager broadcasts for more than channel changes, and a
            // rebuild throws away whichever name editor is being typed in, so
            // only rebuild when the rows would actually come out different.
            if (getChannelCounts != nullptr && getChannelCounts() != builtForCounts)
                rebuildNaming();
        }

        void rebuildNaming()
        {
            if (getChannelCounts != nullptr)
                builtForCounts = getChannelCounts();

            // The viewport does not own the section: detach before replacing it.
            namingViewport.setViewedComponent (nullptr, false);
            naming.reset (createNaming != nullptr ? createNaming() : nullptr);

            if (naming != nullptr)
                namingViewport.setViewedComponent (naming.get(), false);

            resized();
        }

        juce::AudioDeviceManager& deviceManager;
        juce::AudioDeviceSelectorComponent selector;
        juce::Viewport namingViewport;
        std::unique_ptr<juce::Component> naming;
        juce::TextButton doneButton { "Done" };

        std::function<std::pair<int, int>()> getChannelCounts;
        std::function<juce::Component*()> createNaming;
        std::pair<int, int> builtForCounts { -1, -1 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceSetupPanel)
    };

    /** Channel naming + reference routing. One row per channel: an editor for
        the name, and for inputs a combo choosing which output it is compared
        against. Edits are pushed back through the callbacks as they happen. */
    class ChannelSetupPanel : public juce::Component
    {
    public:
        /** numOutputs / numInputs are the device's active channel counts: one
            row each, so the rows match the selection above them. The arrays may
            be longer (they keep names from larger devices); those entries are
            left alone. */
        ChannelSetupPanel (juce::StringArray& outNames, juce::StringArray& inNames,
                           juce::Array<int>& refs, juce::Array<bool>& monitored,
                           int numOutputs, int numInputs,
                           std::function<void()> onChanged)
            : outputNames (outNames), inputNames (inNames),
              referenceForInput (refs), monitoredInput (monitored),
              numOutputRows (juce::jlimit (0, outNames.size(), numOutputs)),
              numInputRows  (juce::jlimit (0, inNames.size(),  numInputs)),
              changed (std::move (onChanged))
        {
            addAndMakeVisible (heading);
            heading.setText ("Name each channel. Untick Monitor for inputs with nothing patched in, "
                             "so they are not reported as permanent dropouts.",
                             juce::dontSendNotification);
            heading.setJustificationType (juce::Justification::centredLeft);

            buildRows();
            // No upper clamp: the panel lives in a scrolling viewport, and
            // clamping made channels past ~18 impossible to name or un-monitor.
            setSize (660, juce::jmax (220, 96 + 30 * (numOutputRows + numInputRows + 2)));
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced (10);
            heading.setBounds (bounds.removeFromTop (34));

            for (auto* row : rows)
            {
                auto r = bounds.removeFromTop (26);
                row->label->setBounds (r.removeFromLeft (90));
                row->editor->setBounds (r.removeFromLeft (200).reduced (0, 2));

                if (row->monitor != nullptr)
                {
                    r.removeFromLeft (8);
                    row->monitor->setBounds (r.removeFromLeft (86).reduced (0, 2));
                }

                if (row->reference != nullptr)
                {
                    r.removeFromLeft (8);
                    row->reference->setBounds (r.reduced (0, 2));
                }

                bounds.removeFromTop (4);
            }
        }

    private:
        struct Row
        {
            std::unique_ptr<juce::Label> label;
            std::unique_ptr<juce::TextEditor> editor;
            std::unique_ptr<juce::ComboBox> reference;      // inputs only
            std::unique_ptr<juce::ToggleButton> monitor;    // inputs only
        };

        void buildRows()
        {
            const auto addRow = [this] (const juce::String& caption, juce::StringArray& names,
                                        int index, bool isInput)
            {
                auto row = std::make_unique<Row>();

                row->label = std::make_unique<juce::Label>();
                row->label->setText (caption, juce::dontSendNotification);
                addAndMakeVisible (*row->label);

                row->editor = std::make_unique<juce::TextEditor>();
                row->editor->setText (names[index], juce::dontSendNotification);
                row->editor->onTextChange = [this, &names, index, ed = row->editor.get()]
                {
                    names.set (index, ed->getText());
                    if (changed) changed();
                };
                addAndMakeVisible (*row->editor);

                if (isInput)
                {
                    row->monitor = std::make_unique<juce::ToggleButton> ("Monitor");
                    while (monitoredInput.size() <= index)
                        monitoredInput.add (true);

                    row->monitor->setToggleState (monitoredInput[index], juce::dontSendNotification);
                    row->monitor->onClick = [this, index, tb = row->monitor.get()]
                    {
                        monitoredInput.set (index, tb->getToggleState());
                        if (changed) changed();
                    };
                    addAndMakeVisible (*row->monitor);

                    row->reference = std::make_unique<juce::ComboBox>();
                    for (int o = 0; o < numOutputRows; ++o)
                        row->reference->addItem ("ref: " + outputNames[o], o + 1);

                    const int current = juce::isPositiveAndBelow (index, referenceForInput.size())
                                          ? referenceForInput[index] : 0;
                    row->reference->setSelectedId (juce::jlimit (1, juce::jmax (1, numOutputRows), current + 1),
                                                   juce::dontSendNotification);

                    row->reference->onChange = [this, index, cb = row->reference.get()]
                    {
                        while (referenceForInput.size() <= index)
                            referenceForInput.add (0);

                        referenceForInput.set (index, juce::jmax (0, cb->getSelectedId() - 1));
                        if (changed) changed();
                    };
                    addAndMakeVisible (*row->reference);
                }

                rows.add (row.release());
            };

            for (int i = 0; i < numOutputRows; ++i)
                addRow ("Output " + juce::String (i + 1), outputNames, i, false);

            for (int i = 0; i < numInputRows; ++i)
                addRow ("Input " + juce::String (i + 1), inputNames, i, true);
        }

        juce::StringArray& outputNames;
        juce::StringArray& inputNames;
        juce::Array<int>&  referenceForInput;
        juce::Array<bool>& monitoredInput;
        const int numOutputRows, numInputRows;
        std::function<void()> changed;

        juce::Label heading;
        juce::OwnedArray<Row> rows;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelSetupPanel)
    };

    /** Minimal modeless settings dialog content: EventDetector thresholds,
        NotificationEngine tuning, pink-noise seed (persisted; applied at the
        next Start Audio since PinkNoiseGenerator::setSeed() is non-real-time),
        and the log base folder. Deliberately self-contained so MainComponent
        doesn't need any new members beyond what MainComponent.h declares. */
    class SettingsPanel : public juce::Component
    {
    public:
        SettingsPanel (EventDetector& detectorIn, ArtifactDetector& artifactsIn, NotificationEngine& notifierIn,
                       juce::ApplicationProperties& propsIn)
            : detector (detectorIn), artifacts (artifactsIn), notifier (notifierIn), props (propsIn)
        {
            const auto thresholds = detector.getThresholds();

            // A dropout is judged against what the return SHOULD be: this many
            // dB below the level the reference and the learned path gain predict.
            setUpLabel (dropoutRelativeLabel, "Dropout: dB below expected");
            setUpSlider (dropoutRelativeSlider, 6.0, 60.0, 1.0, thresholds.dropoutRelativeDb);
            dropoutRelativeSlider.onValueChange = [this] { applyThresholds(); };

            // Only used in the first seconds, before the path gain is learned.
            setUpLabel (dropoutLabel, "Dropout floor at start (dBFS)");
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
            notifyToggle.onClick = [this]
            {
                notifier.setEnabled (notifyToggle.getToggleState());
                props.getUserSettings()->setValue ("notificationsEnabled", notifyToggle.getToggleState());
            };

            setUpLabel (cooldownLabel, "Notification cooldown (s)");
            setUpSlider (cooldownSlider, 5.0, 600.0, 5.0, notifier.getCooldownSeconds());
            cooldownSlider.onValueChange = [this]
            {
                notifier.setCooldownSeconds (cooldownSlider.getValue());
                props.getUserSettings()->setValue ("notificationCooldownSeconds", cooldownSlider.getValue());
            };

            setUpLabel (seedLabel, "Pink noise seed (0 = auto, applies on next Start Audio)");
            addAndMakeVisible (seedEditor);
            seedEditor.setInputRestrictions (20, "0123456789");
            seedEditor.setText (props.getUserSettings()->getValue ("pinkNoiseSeed", "0"), juce::dontSendNotification);
            seedEditor.onFocusLost = [this] { props.getUserSettings()->setValue ("pinkNoiseSeed", seedEditor.getText()); };
            seedEditor.onReturnKey = [this] { props.getUserSettings()->setValue ("pinkNoiseSeed", seedEditor.getText()); };

            addAndMakeVisible (recaptureButton);
            recaptureButton.onClick = [this]
            {
                detector.recaptureBaseline();
                artifacts.recaptureBaseline();
            };

            // Click / noise / distortion / timing-slip detection. On unless the
            // user turns it off (e.g. a path with heavy processing in it, where
            // the return is deliberately not a copy of the reference).
            addAndMakeVisible (artifactToggle);
            artifactToggle.setToggleState (artifacts.getSettings().enabled, juce::dontSendNotification);
            artifactToggle.onClick = [this]
            {
                auto settings = artifacts.getSettings();
                settings.enabled = artifactToggle.getToggleState();
                artifacts.setSettings (settings);
                props.getUserSettings()->setValue ("artifactDetectionEnabled", settings.enabled);
                props.getUserSettings()->saveIfNeeded();
            };

            addAndMakeVisible (chooseFolderButton);
            chooseFolderButton.onClick = [this] { chooseLogFolder(); };

            // Per-session prompting vs one fixed folder for every log.
            addAndMakeVisible (promptEachToggle);
            promptEachToggle.setToggleState (props.getUserSettings()
                                                 ->getBoolValue ("promptLogEachSession", false),
                                             juce::dontSendNotification);
            promptEachToggle.onClick = [this]
            {
                props.getUserSettings()->setValue ("promptLogEachSession",
                                                   promptEachToggle.getToggleState());
                props.getUserSettings()->saveIfNeeded();
            };

            setSize (460, 410);
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
            t.dropoutRelativeDb      = (float) dropoutRelativeSlider.getValue();
            t.levelChangeToleranceDb = (float) levelTolSlider.getValue();
            t.minDropoutMs           = minDropoutSlider.getValue();
            detector.setThresholds (t);

            // Persisted: these are the rig's calibration, and they used to fall
            // back to the defaults at every launch with nothing to say so.
            // (MainComponent's ctor reads the same keys back.)
            auto* settings = props.getUserSettings();
            settings->setValue ("dropoutThresholdDb",     dropoutSlider.getValue());
            settings->setValue ("levelChangeToleranceDb", levelTolSlider.getValue());
            settings->setValue ("minDropoutMs",           minDropoutSlider.getValue());
            settings->setValue ("dropoutRelativeDb",      dropoutRelativeSlider.getValue());
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
                label.setBounds (r.removeFromLeft (200));
                control.setBounds (r);
                b.removeFromTop (6);
            };

            row (dropoutRelativeLabel, dropoutRelativeSlider);
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
            b.removeFromTop (8);
            promptEachToggle.setBounds (b.removeFromTop (24));
            b.removeFromTop (8);
            artifactToggle.setBounds (b.removeFromTop (24));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::get (Palette::panelBackground));
        }

        EventDetector& detector;
        ArtifactDetector& artifacts;
        NotificationEngine& notifier;
        juce::ApplicationProperties& props;

        juce::Label dropoutRelativeLabel, dropoutLabel, levelTolLabel, minDropoutLabel, notifyToggleLabel, cooldownLabel, seedLabel;
        juce::Slider dropoutRelativeSlider, dropoutSlider, levelTolSlider, minDropoutSlider, cooldownSlider;
        juce::ToggleButton notifyToggle;
        juce::TextEditor seedEditor;
        juce::TextButton recaptureButton  { "Recapture Level Baseline" };
        juce::ToggleButton promptEachToggle { "Ask for log name & location at each session start" };
        juce::ToggleButton artifactToggle { "Detect clicks, noise, distortion and timing slips" };
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

    // Rows the logger writes on its own account (clock re-anchored after a
    // sleep, writing recovered after an error) belong on screen as well.
    sessionLogger.onInternalEvent = [this] (const Event& e, const juce::String& description)
    {
        eventLog.addEvent (e, description, sessionStartUtc + juce::RelativeTime (e.elapsedSeconds));
    };

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

        // One alert per failure. A failed start is alerted by
        // beginLoggingSession, which wants this reason for it; anything else is
        // a write failing mid-run, alerted once however many rows and files
        // then fail behind it (reconcileStatus re-arms it once writing recovers).
        if (startingLoggingSession)
        {
            loggingStartError = message;
        }
        else if (! writeFailureAlertShown)
        {
            writeFailureAlertShown = true;
            showErrorAlert ("Logging is losing data",
                            message + "\n\nThe log could not be written, so measurement rows are being "
                              "lost. The app retries every few seconds; when writing resumes, "
                              "events.csv records it and how many rows were lost.\n\nSession folder: "
                              + sessionLogger.getSessionFolder().getFullPathName()
                              + "\n\nCheck the disk the log is on.");
        }
    };

    // Detection thresholds and notification settings are part of this rig's
    // calibration: restore them before the detector sees its first block (the
    // device callback is attached by initialiseDevices just below) and before
    // the Settings dialog can read them back.
    {
        auto* settings = appProperties.getUserSettings();
        auto thresholds = engine->getDetector().getThresholds();

        // Only what has actually been saved, clamped to the range its Settings
        // slider offers; anything never saved keeps the built-in default.
        if (settings->containsKey ("dropoutThresholdDb"))
            thresholds.dropoutThresholdDb = (float) juce::jlimit (-80.0, -20.0,
                settings->getDoubleValue ("dropoutThresholdDb"));

        if (settings->containsKey ("levelChangeToleranceDb"))
            thresholds.levelChangeToleranceDb = (float) juce::jlimit (1.0, 24.0,
                settings->getDoubleValue ("levelChangeToleranceDb"));

        if (settings->containsKey ("minDropoutMs"))
            thresholds.minDropoutMs = juce::jlimit (10.0, 2000.0,
                settings->getDoubleValue ("minDropoutMs"));

        if (settings->containsKey ("dropoutRelativeDb"))
            thresholds.dropoutRelativeDb = (float) juce::jlimit (0.0, 120.0,
                settings->getDoubleValue ("dropoutRelativeDb"));

        engine->getDetector().setThresholds (thresholds);

        if (settings->containsKey ("artifactDetectionEnabled"))
        {
            auto artifactSettings = engine->getArtifactDetector().getSettings();
            artifactSettings.enabled = settings->getBoolValue ("artifactDetectionEnabled");
            engine->getArtifactDetector().setSettings (artifactSettings);
        }

        if (settings->containsKey ("notificationsEnabled"))
            notificationEngine.setEnabled (settings->getBoolValue ("notificationsEnabled"));

        if (settings->containsKey ("notificationCooldownSeconds"))
            notificationEngine.setCooldownSeconds (juce::jlimit (5.0, 600.0,
                settings->getDoubleValue ("notificationCooldownSeconds")));
    }

    notificationEngine.initialise();

    // Open the device up front (restoring the user's last selection) so Audio
    // Device Setup edits a live device before Start Audio is ever pressed, and
    // so the status bar shows the real interface immediately.
    {
        auto savedState = appProperties.getUserSettings()->getXmlValue ("audioDeviceState");
        engine->initialiseDevices (savedState.get());

        // The saved state is what the user chose, whatever initialise() managed
        // to open: if that interface is missing JUCE quietly opens the system
        // default instead, and reports no error for it.
        setChosenDeviceState (std::move (savedState));
    }

    // Baseline for change reporting, captured before we start listening so the
    // initial device selection isn't reported as a "change".
    lastKnownSetup = engine->getDeviceManager().getAudioDeviceSetup();

    observedSetup        = lastKnownSetup;
    observedManagerState = engine->createDeviceStateXml();

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


    statusBar.onCellClicked = [this] (TopStatusBar::CellId id, juce::Rectangle<int> cellBounds)
    {
        showStatusBarDeviceMenu (id, cellBounds);
    };

    loadChannelConfig();
    refreshChannelConfig();
    loadWaveformConfig();

    waveform.onLaneConfigChanged = [this] { saveWaveformConfig(); };

    // Renaming a marker updates the live log immediately; push the new name
    // into events.csv too while the session is still open.
    eventLog.onMarkerRenamed = [this] (const Event& marker, const juce::String& newName)
    {
        // false = the name exists only in the live log (no session, or the
        // marker was never written to this one); the row then says so.
        return sessionLogger.isSessionActive()
            && sessionLogger.amendEventDescription (marker.elapsedSeconds, marker.type, newName);
    };

    // Notes work on every event — "changing the patch cable here" next to a
    // dropout tells future-you it wasn't a real fault.
    eventLog.onNoteEdited = [this] (const Event& e, const juce::String& note)
    {
        return sessionLogger.isSessionActive()
            && sessionLogger.amendEventNote (e, note);
    };

    // Restore the last output level so an overnight rig comes back at the level
    // it was calibrated to, not the default.
    {
        const double savedDb = appProperties.getUserSettings()
                                   ->getDoubleValue ("outputLevelDb", PinkNoiseGenerator::defaultLevelDb);
        engine->getGenerator().setLevelDb (savedDb);
        transport.setOutputLevelDb (savedDb);
    }

    wireTransport();

    statusBar.setTestMode ("Pink noise loopback");
    statusBar.setAppStatus ("Idle");

    setSize (1120, 720);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();

    // Nothing may ask or report from here on: close any open alert/confirm
    // (their callbacks are not called) and stop write errors raising new ones.
    confirmWindow.deleteAndZero();
    errorBox.close();
    sessionLogger.onLoggingError = nullptr;
    sessionLogger.onInternalEvent = nullptr;
    onTransportStateChanged = nullptr;

    engine->getDeviceManager().removeChangeListener (this);
    saveDeviceState();

    if (settingsWindow != nullptr)
        settingsWindow.deleteAndZero();

    if (deviceSetupWindow != nullptr)
        deviceSetupWindow.deleteAndZero();

    // Stops logging (if active) and the audio engine (fades out first).
    stopAudio();
    releaseSleepAssertion();

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

    // The meter column grows with the channel count: at four bars a side the old
    // fixed 200 px left each bar a few pixels wide, which made the per-channel
    // names unreadable. Width is capped so the waveform keeps most of the window.
    const int refBars   = juce::jmax (1, refMeter.getNumChannels());
    const int inputBars = juce::jmax (1, inputMeter.getNumChannels());
    const int perBar    = 26;

    const int wantedWidth = 60 + perBar * (refBars + inputBars);
    const int meterWidth  = juce::jlimit (200, juce::jmax (200, area.getWidth() / 2), wantedWidth);

    auto meterColumn = area.removeFromLeft (meterWidth);
    area.removeFromLeft (8);
    waveform.setBounds (area);

    // Split the column in proportion to how many bars each meter has to draw.
    const int refWidth = juce::roundToInt ((float) (meterColumn.getWidth() - 4)
                                             * (float) refBars / (float) (refBars + inputBars));

    auto refBounds = meterColumn.removeFromLeft (juce::jmax (60, refWidth));
    meterColumn.removeFromLeft (4);
    refMeter.setBounds (refBounds);
    inputMeter.setBounds (meterColumn);
}

//==============================================================================
void MainComponent::timerCallback()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();

    // The logger dates rows from the audio clock; telling it where that clock
    // is now lets it see at once when a row arrives after a stall or a sleep.
    sessionLogger.setLiveAudioClock (engine->getAnalysis().getElapsedSeconds());

    // The expected-change window (see expectDeviceChangeFor) has lapsed: from
    // here a device stop is a fault again. Anything that died inside the window
    // went unreported by the engine, so check the device is still there.
    if (expectedChangeUntilMs > 0.0 && nowMs >= expectedChangeUntilMs)
    {
        endExpectedDeviceChange();

        // Reported, never acted on: a lapse must not be able to end a session.
        // (reconcileStatus, further down this tick, deals with a device that
        // was swapped inside the window rather than lost.)
        checkDeviceStillPresent (false);
    }

    // Before the drain: it decides whether a device-lost event waiting in the
    // queue was the user's own change.
    trackDeviceSelection();

    drainMeasurementQueue();
    drainEventQueue();
    updateMetersAndWaveform();

    checkDataFlow (nowMs);
    recoverChosenDevice (nowMs);
    reconcileStatus (nowMs);
    updateStatusBar();

    // The menu's item names and enablement follow the real engine/logger state,
    // whatever changed it (a button, the menu, a device loss, a confirm box).
    const bool audioNow   = engine->isRunning();
    const bool loggingNow = sessionLogger.isSessionActive();

    if (audioNow != reportedAudioRunning || loggingNow != reportedLoggingActive)
    {
        reportedAudioRunning  = audioNow;
        reportedLoggingActive = loggingNow;

        if (onTransportStateChanged != nullptr)
            onTransportStateChanged();
    }
}

void MainComponent::drainMeasurementQueue()
{
    const int numDrained = measurementQueue.popAll ([this] (const Measurement& m)
    {
        // Measurement now carries a per-channel breakdown, so light exactly the
        // channels that actually clipped rather than the first two.
        if (m.clipCountBlock > 0)
            for (int ch = 0; ch < juce::jlimit (0, maxChannels, m.numInputChannels); ++ch)
                if (m.clipCountPerChannel[ch] > 0)
                    inputMeter.notifyClipped (ch);

        if (sessionLogger.isSessionActive() && ! loggingPaused)
            sessionLogger.logMeasurement (m);
    });

    // Stall watchdog (see checkDataFlow): measurements are flowing.
    if (numDrained > 0)
    {
        if (stallActive)
        {
            stallActive = false;

            // Wall clock for the length: the monotonic counter does not advance
            // while the Mac is asleep, which is one of the ways data stops.
            const auto gap = (juce::Time::getCurrentTime() - lastMeasurementWall).inSeconds();
            logSessionNote ("Audio data resumed after " + juce::String (gap, 1)
                              + " s without measurements", Severity::Info);
        }

        lastMeasurementMs   = juce::Time::getMillisecondCounterHiRes();
        lastMeasurementWall = juce::Time::getCurrentTime();
    }
}

void MainComponent::drainEventQueue()
{
    eventQueue.popAll ([this] (const Event& e)
    {
        // The engine cannot tell a device stop the user caused in Audio Device
        // Setup from a failure, and reports both. trackDeviceSelection() can
        // (the saved selection changed), and opens this window when it does.
        if (e.type == EventType::AudioDeviceLost
            && juce::Time::getMillisecondCounterHiRes() < swallowDeviceEventsUntilMs)
            return;

        const auto description = SessionLogger::composeDescription (e);
        const auto wallClockUtc = sessionStartUtc + juce::RelativeTime (e.elapsedSeconds);

        eventLog.addEvent (e, description, wallClockUtc);

        if (e.type == EventType::Clipping && e.code == eventCodeStart)
        {
            // channelIndex is authoritative; `channel` is always Both now.
            if (e.channelIndex >= 0 && e.channelIsInput)
                inputMeter.notifyClipped (e.channelIndex);
        }

        // Not gated by pause: pausing holds back measurement rows only. A fault
        // during a pause still belongs in the record.
        if (sessionLogger.isSessionActive())
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

        // Feed each lane whichever channel it is currently set to show.
        for (int lane = 0; lane < waveform.getNumLanes(); ++lane)
        {
            const auto src = waveform.getLaneSource (lane);

            if (! src.active)
                continue;

            const int got = src.isInput
                              ? engine->readInputWaveform (src.channel, waveformScratch.data(), samplesToRead)
                              : engine->readReferenceWaveform (src.channel, waveformScratch.data(), samplesToRead);

            // waveformScratch is shared across lanes this tick, so pushing more
            // than was written would feed this lane the previous lane's samples.
            if (got > 0)
                waveform.pushSamples (lane, waveformScratch.data(), got);
        }
    }

    waveform.refresh();
}

void MainComponent::updateStatusBar()
{
    statusBar.setSessionDuration (engine->isRunning() ? engine->getAnalysis().getElapsedSeconds() : -1.0);

    // A forgotten pause is a silent hole in the log: count it up in the LOG cell.
    if (loggingPaused)
        statusBar.setLoggingPausedSeconds ((double) (juce::Time::getMillisecondCounter() - pauseStartedMs) / 1000.0);

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

    const juce::String title = juce::String ("Pink Noise Monitor: ") + toString (e.type);
    notificationEngine.postCritical (title, description, e.type);
}

//==============================================================================
void MainComponent::wireTransport()
{
    transport.onStartAudio  = [this] { startAudio(); };
    transport.onStopAudio   = [this] { requestStopAudio(); };

    transport.onStartLogging = [this] { startLogging(); };
    transport.onStopLogging  = [this] { requestStopLogging(); };
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

    auto& deviceManager = engine->getDeviceManager();

    const auto reportStartFailure = [this]
    {
        transport.setAudioRunning (false);
        statusBar.setAudioEngineState (TopStatusBar::EngineState::deviceError);
        statusBar.setAppStatus ("Failed to start audio device");

        showErrorAlert ("Audio did not start",
                        "No audio device could be opened, so nothing is playing and nothing is "
                        "being measured.\n\nCheck the interface is connected and powered, then "
                        "choose it in Audio Device Setup (Cmd-Shift-A) and press Start Audio again.");
    };

    // Nothing is open (the interface was off at launch, or everything has gone
    // since), or a device is open that the engine is not running on — one
    // picked in Audio Device Setup after a launch that found no device at all.
    // In both cases engine->start() would open a device of its own choosing,
    // which is the system default: open the saved interface here instead, so
    // that what start() finds is a device this function has already looked at.
    if (chosenDeviceState != nullptr
        && (deviceManager.getCurrentAudioDevice() == nullptr || ! engine->isDeviceActive()))
    {
        expectDeviceChangeFor (1000.0);
        engine->initialiseDevices (chosenDeviceState.get());
        endExpectedDeviceChange();

        observedSetup        = deviceManager.getAudioDeviceSetup();
        observedManagerState = engine->createDeviceStateXml();

        if (deviceManager.getCurrentAudioDevice() == nullptr)
        {
            reportStartFailure();
            return;
        }
    }

    // The interface the user set this rig up with is not the one that is open:
    // JUCE fell back to the system default. Never start noise through that
    // without asking. Saying yes covers this device and this run only; the
    // saved interface stays saved and is switched back to when it reappears.
    if (isOnUnchosenFallback() && ! isLiveDeviceAccepted())
    {
        const auto saved = describeDevices (chosenOutputName, chosenInputName);
        const auto live  = describeDevices (observedSetup.outputDeviceName, observedSetup.inputDeviceName);

        showConfirm (juce::MessageBoxIconType::WarningIcon,
                     "Saved interface not found",
                     "Saved interface '" + saved + "' was not found. Play through '"
                       + live + "' instead?\n\n'" + saved + "' stays saved. The app switches "
                         "back to it when it reappears, unless a session started on '"
                       + live + "' is still running.",
                     "Cancel", "Play Through It",
                     [this]
                     {
                         acceptLiveDevice();
                         startAudio();
                     });

        return;
    }

    statusBar.setAudioEngineState (TopStatusBar::EngineState::starting);

    // Anything still queued belongs to the clock that start() is about to zero:
    // put it in the live log now, so every row from here on is this session's.
    drainEventQueue();
    audioSessionFirstEvent = eventLog.getTotalEventsReceived();

    // Apply a user-chosen seed (persisted via Settings) before starting;
    // PinkNoiseGenerator::setSeed() is non-real-time and must be called while
    // audio is stopped, which is guaranteed here.
    const juce::String seedText = appProperties.getUserSettings()->getValue ("pinkNoiseSeed", "0");
    const uint64_t seed = PinkNoiseGenerator::parseSeed (seedText);   // unsigned: a signed parse loses seeds >= 2^63
    if (seed != 0)
        engine->getGenerator().setSeed (seed);

    if (engine->start())
    {
        sessionStartUtc = juce::Time::getCurrentTime();

        // The stall watchdog counts from here, not from the last idle block.
        lastMeasurementMs   = juce::Time::getMillisecondCounterHiRes();
        lastMeasurementWall = sessionStartUtc;
        stallActive         = false;

        // With no interface saved there is nothing to fall back FROM, so the
        // device the user just pressed Start on is the one they mean: from here
        // on, ending up on any other is treated like a fallback.
        observedSetup = deviceManager.getAudioDeviceSetup();

        if (chosenDeviceState == nullptr)
            acceptLiveDevice();

        waveform.setSampleRate (engine->getSampleRate());

        // Re-derive meters/names from the live device rather than forcing a
        // stereo count here — this used to collapse an 8-input meter to 2 bars.
        refreshChannelConfig();

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
        reportStartFailure();
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

    // A log name + folder is required. Prompt when nothing has ever been set,
    // or every time if the user prefers to pick a destination per session.
    // Either way the chooser continues into beginLoggingSession directly —
    // never back into startLogging, which would re-prompt forever.
    const bool promptEverySession = appProperties.getUserSettings()
                                        ->getBoolValue ("promptLogEachSession", false);

    if (promptEverySession || ! isLogDestinationConfigured())
    {
        chooseLogDestination ([this] (bool chosen)
        {
            if (chosen)
                runLoggingPreflight();
        });

        return;
    }

    runLoggingPreflight();
}

void MainComponent::runLoggingPreflight()
{
    if (! engine->isRunning() || sessionLogger.isSessionActive())
        return;

    const auto problems = collectPreflightProblems();

    // Nothing wrong: no dialog, logging just starts.
    if (problems.isEmpty())
    {
        beginLoggingSession();
        return;
    }

    showConfirm (juce::MessageBoxIconType::WarningIcon,
                 "Check before you start logging",
                 problems.joinIntoString ("\n\n") + "\n\nStart logging anyway?",
                 "Cancel", "Start Logging",
                 [this]
                 {
                     if (! sessionLogger.isSessionActive())
                         beginLoggingSession();
                 });
}

juce::StringArray MainComponent::collectPreflightProblems()
{
    juce::StringArray problems;

    // 1. Muted output: the detector is suspended, so the run would log as clean
    //    whatever happened to the signal path.
    if (engine->getGenerator().isMuted())
        problems.add ("- The output is muted. Fault detection is suspended while muted, so the "
                      "log would show a clean run whatever happens.");

    // 2. Monitored inputs at digital silence. A patched analogue input always
    //    carries some noise; exactly nothing means the input is not reaching the
    //    app at all, which is also what a denied microphone permission produces
    //    on every input at once.
    int outs = 0, ins = 0;
    getChannelCounts (outs, ins);

    juce::StringArray silentInputs;
    int numMonitored = 0;

    for (int ch = 0; ch < ins; ++ch)
    {
        if (ch < inputMonitored.size() && ! inputMonitored[ch])
            continue;

        ++numMonitored;

        if (engine->getAnalysis().getInPeakDb (ch) <= silenceFloorDb)
            silentInputs.add (juce::isPositiveAndBelow (ch, inputChannelNames.size())
                                && inputChannelNames[ch].isNotEmpty() ? inputChannelNames[ch]
                                                                       : "In " + juce::String (ch + 1));
    }

    if (numMonitored == 0)
        problems.add ("- No input is being monitored (no input channels are enabled, or Monitor "
                      "is unticked on all of them), so no fault can be detected.");
    else if (! silentInputs.isEmpty())
        problems.add ("- Digital silence on: " + silentInputs.joinIntoString (", ")
                      + ". A patched input is never exactly silent, so this input is not reaching "
                        "the app. It is also what every input reads when macOS microphone "
                        "permission is denied (System Settings > Privacy & Security > Microphone).");

    // 3. Faults already active. The detector reports each one once, when it
    //    starts, so these are listed here and copied into the new events.csv.
    const auto activeFaults = findActiveFaultRows();

    if (! activeFaults.empty())
    {
        juce::StringArray named;

        for (const auto& row : activeFaults)
        {
            if (named.size() == 6)
            {
                named.add ("...");
                break;
            }

            const auto& names = row.event.channelIsInput ? inputChannelNames : outputChannelNames;
            const int index   = row.event.channelIndex;

            named.add (juce::String (toString (row.event.type))
                       + (index < 0 ? juce::String()
                                    : " on " + (juce::isPositiveAndBelow (index, names.size())
                                                  && names[index].isNotEmpty()
                                                    ? names[index]
                                                    : (row.event.channelIsInput ? "In " : "Out ")
                                                        + juce::String (index + 1))));
        }

        problems.add ("- " + juce::String ((int) activeFaults.size())
                      + (activeFaults.size() == 1 ? " fault is" : " faults are")
                      + " already active: " + named.joinIntoString (", ")
                      + ". Each will be written to events.csv marked as already active.");
    }
    else if (engine->getDetector().isDropoutActive() || engine->getDetector().isClippingActive())
    {
        problems.add ("- A fault is already active (its start is no longer in the event log). "
                      "It will be noted in events.csv.");
    }

    if (! engine->isDeviceActive() || engine->isOutputSilencedForDeviceLoss())
        problems.add ("- The audio device is lost and the output is silent. Nothing is being "
                      "tested until it returns.");

    // 4. Destination. Start Logging would fail outright on an unwritable folder;
    //    a nearly full volume fails later, when nobody is watching.
    const auto folder = getLogBaseFolder();

    if (! folder.hasWriteAccess())
    {
        problems.add ("- The log folder cannot be written to: " + folder.getFullPathName());
    }
    else
    {
        // The folder may not exist yet; ask about the volume its parent is on.
        auto existing = folder;

        while (! existing.exists() && existing.getParentDirectory() != existing)
            existing = existing.getParentDirectory();

        constexpr juce::int64 oneGigabyte = 1024LL * 1024LL * 1024LL;
        const auto freeBytes = existing.getBytesFreeOnVolume();

        if (freeBytes < oneGigabyte)
            problems.add ("- Only " + juce::String ((double) freeBytes / (double) oneGigabyte, 2)
                          + " GB free on the log volume (" + folder.getFullPathName() + ").");
    }

    return problems;
}

std::vector<MainComponent::LiveLogRow> MainComponent::findActiveFaultRows()
{
    std::vector<LiveLogRow> open;

    // Only rows from the current audio session: start() wipes the detector, so
    // nothing older can still be active (and older rows are on another clock).
    eventLog.visitEventsSince (audioSessionFirstEvent,
                               [&open] (const Event& e, const juce::String& description, const juce::String& note)
    {
        if (e.code == eventCodeStart)
        {
            open.push_back ({ e, description, note });
        }
        else if (e.code == eventCodeEnd)
        {
            // An end closes the start of the same condition on the same channel.
            open.erase (std::remove_if (open.begin(), open.end(), [&e] (const LiveLogRow& row)
                        {
                            return row.event.type == e.type
                                && row.event.channelIndex == e.channelIndex
                                && row.event.channelIsInput == e.channelIsInput;
                        }),
                        open.end());
        }

        return false;   // only looking: nothing here has been written anywhere
    });

    return open;
}

void MainComponent::beginLoggingSession()
{
    if (! engine->isRunning())
        return;

    // Bring the live log right up to date, then note what is still wrong: a
    // fault that began before logging did has already had its one start event.
    drainEventQueue();
    const auto activeFaults = findActiveFaultRows();

    markerCounter = 0;
    currentSessionInfo = buildSessionInfo();

    loggingStartError.clear();
    writeFailureAlertShown = false;

    startingLoggingSession = true;
    const bool started = sessionLogger.startSession (getLogBaseFolder(), currentSessionInfo);
    startingLoggingSession = false;

    if (started)
    {
        loggingPaused = false;
        transport.setLoggingActive (true);
        transport.setLoggingPaused (false);
        statusBar.setLoggingState (TopStatusBar::LoggingState::active);

        sessionAvailable = true;
        transport.setSessionAvailable (true);
        statusBar.setAppStatus ("Logging: " + currentSessionInfo.sessionId);

        holdSleepAssertion();

        // Copy the still-active faults into the new events.csv. Without this a
        // dead input that dropped out before Start Logging left events.csv
        // empty all night. Each row keeps its real start time (same session
        // clock), so it sorts ahead of everything this session records, and
        // its end event will follow it in the normal way.
        bool dropoutReplayed = false, clippingReplayed = false;

        for (const auto& row : activeFaults)
        {
            sessionLogger.logEvent (row.event, row.description + " [already active when logging started]");

            if (row.note.isNotEmpty())
                sessionLogger.amendEventNote (row.event, row.note);

            dropoutReplayed  = dropoutReplayed  || row.event.type == EventType::CompleteDropout;
            clippingReplayed = clippingReplayed || row.event.type == EventType::Clipping;
        }

        // The live log may have been cleared (or have dropped the row) since
        // the fault began. The detector still knows, if not which channel.
        if (engine->getDetector().isDropoutActive() && ! dropoutReplayed)
            logSessionNote ("A dropout was already active when logging started "
                            "(its start is no longer in the event log)", Severity::Warning);

        if (engine->getDetector().isClippingActive() && ! clippingReplayed)
            logSessionNote ("Clipping was already active when logging started "
                            "(its start is no longer in the event log)", Severity::Warning);

        if (! engine->isDeviceActive() || engine->isOutputSilencedForDeviceLoss())
            logSessionNote ("The audio device was already lost when logging started "
                            "(output held silent)", Severity::Warning);

        // Those rows, and any note on them, are in the CSV now.
        eventLog.visitEventsSince (audioSessionFirstEvent,
                                   [&activeFaults] (const Event& e, const juce::String&, const juce::String&)
        {
            return std::any_of (activeFaults.begin(), activeFaults.end(), [&e] (const LiveLogRow& row)
            {
                return row.event.type == e.type
                    && row.event.code == e.code
                    && row.event.channelIndex == e.channelIndex
                    && row.event.channelIsInput == e.channelIsInput
                    && juce::approximatelyEqual (row.event.elapsedSeconds, e.elapsedSeconds);
            });
        });
    }
    else
    {
        statusBar.setLoggingState (TopStatusBar::LoggingState::writeError);
        statusBar.setAppStatus ("Could not start logging session");

        showErrorAlert ("Logging did not start",
                        (loggingStartError.isNotEmpty() ? loggingStartError
                                                        : juce::String ("The session files could not be created."))
                          + "\n\nLog folder: " + getLogBaseFolder().getFullPathName()
                          + "\n\nNothing is being recorded. Choose another destination with "
                            "Logging > Set Log File (Cmd-Shift-L), then press Start Logging again.");
    }
}

void MainComponent::stopLogging()
{
    if (! sessionLogger.isSessionActive())
        return;

    sessionLogger.stopSession();
    currentSessionInfo = sessionLogger.getSessionInfo();
    releaseSleepAssertion();

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
    if (! sessionLogger.isSessionActive() || loggingPaused == shouldPause)
        return;

    loggingPaused = shouldPause;

    // The pause is itself part of the record: without these two rows the gap it
    // leaves in measurements.csv is indistinguishable from lost data.
    if (loggingPaused)
    {
        pauseStartedMs = juce::Time::getMillisecondCounter();
        logSessionNote ("Logging paused (measurements are not recorded until it is resumed)", Severity::Info);
    }
    else
    {
        const auto pausedFor = juce::RelativeTime::milliseconds (
            (juce::int64) (juce::Time::getMillisecondCounter() - pauseStartedMs));

        logSessionNote ("Logging resumed (paused for " + pausedFor.getDescription ("0 secs") + ")", Severity::Info);
    }

    transport.setLoggingPaused (loggingPaused);

    // Same rule as reconcileStatus: an unrecovered write error outranks both.
    statusBar.setLoggingState (sessionLogger.hasUnrecoveredWriteError() ? TopStatusBar::LoggingState::writeError
                               : loggingPaused                           ? TopStatusBar::LoggingState::paused
                                                                         : TopStatusBar::LoggingState::active);
}

void MainComponent::logSessionNote (const juce::String& description, Severity severity)
{
    Event e {};
    e.type           = EventType::Other;
    e.severity       = severity;
    e.channel        = ChannelId::None;
    e.elapsedSeconds = engine->getAnalysis().getElapsedSeconds();
    e.code           = eventCodeInstant;

    // Injected directly (message thread), the same way as a manual marker.
    eventLog.addEvent (e, description, sessionStartUtc + juce::RelativeTime (e.elapsedSeconds));

    if (sessionLogger.isSessionActive())
        sessionLogger.logEvent (e, description);

    maybeNotify (e, description);   // Critical only
}

//==============================================================================
bool MainComponent::isAudioRunning() const
{
    return engine->isRunning();
}

bool MainComponent::isLoggingActive() const
{
    return sessionLogger.isSessionActive();
}

void MainComponent::confirmIfLogging (const juce::String& actionName,
                                      const juce::String& consequence,
                                      std::function<void()> proceed)
{
    if (proceed == nullptr)
        return;

    if (! sessionLogger.isSessionActive())
    {
        proceed();
        return;
    }

    showConfirm (juce::MessageBoxIconType::QuestionIcon,
                 actionName + "?",
                 "Logging is active (" + currentSessionInfo.sessionId + ").\n\n" + consequence,
                 "Keep Logging", actionName,
                 std::move (proceed));
}

void MainComponent::showConfirm (juce::MessageBoxIconType icon,
                                 const juce::String& title, const juce::String& message,
                                 const juce::String& safeButtonText, const juce::String& actionButtonText,
                                 std::function<void()> onAction)
{
    // A question still open is withdrawn: its callback gets the safe answer.
    confirmWindow.deleteAndZero();

    // Built by hand rather than through MessageBoxOptions, whose two-button box
    // makes the FIRST button the Return default and gives each button its
    // initial letter as a bare shortcut — so Return, or a stray "s" or "q",
    // stopped a run. Here the safe button comes first (it also gets the initial
    // keyboard focus), and takes Return and Escape; the action has no key at
    // all and returns the only non-zero code. Closing the box any other way,
    // including this component being destroyed, yields 0.
    auto* box = new juce::AlertWindow (title, message, icon, this);

    box->addButton (safeButtonText, 0,
                    juce::KeyPress (juce::KeyPress::returnKey),
                    juce::KeyPress (juce::KeyPress::escapeKey));
    box->addButton (actionButtonText, 1);

    // The padding LookAndFeel_V4::createAlertWindow adds to the boxes it makes,
    // so this one matches the alerts beside it.
    box->setBounds (box->getBounds().withSizeKeepingCentre (box->getWidth() + 50, box->getHeight() + 50));

    for (auto* child : box->getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
            button->setBounds (button->getBounds() + juce::Point<int> (25, 40));

    confirmWindow = box;

    juce::Component::SafePointer<MainComponent> safeThis (this);

    // Async: no modal loop. The modal manager deletes the box once answered.
    box->enterModalState (true,
                          juce::ModalCallbackFunction::create ([safeThis, action = std::move (onAction)] (int result)
                          {
                              if (result == 1 && safeThis != nullptr && action != nullptr)
                                  action();
                          }),
                          true);
}

void MainComponent::requestStopAudio()
{
    confirmIfLogging ("Stop Audio",
                      "Stopping audio also stops logging and closes this session's CSV files.",
                      [this] { stopAudio(); });
}

void MainComponent::requestStopLogging()
{
    confirmIfLogging ("Stop Logging",
                      "This closes the session's CSV files. Starting again begins a new session folder.",
                      [this] { stopLogging(); });
}

void MainComponent::showErrorAlert (const juce::String& title, const juce::String& message)
{
    errorBox = juce::AlertWindow::showScopedAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle (title)
            .withMessage (message)
            .withButton ("OK")
            .withAssociatedComponent (this),
        nullptr);
}

void MainComponent::holdSleepAssertion()
{
   #if JUCE_MAC
    if (sleepAssertionId != 0)
        return;

    // PreventUserIdleSystemSleep: the display may still sleep, the machine may
    // not. Released in stopLogging(); the system also drops it if the app dies.
    IOPMAssertionID assertion = kIOPMNullAssertionID;

    if (IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleSystemSleep,
                                     kIOPMAssertionLevelOn,
                                     CFSTR ("Pink Noise Audio Monitor is logging"),
                                     &assertion) == kIOReturnSuccess)
    {
        sleepAssertionId = (juce::uint32) assertion;
    }
    else
    {
        logSessionNote ("Could not prevent system sleep for this session; "
                        "check the Mac's Energy settings before leaving it", Severity::Warning);
    }
   #endif
}

void MainComponent::releaseSleepAssertion()
{
   #if JUCE_MAC
    if (sleepAssertionId != 0)
    {
        IOPMAssertionRelease ((IOPMAssertionID) sleepAssertionId);
        sleepAssertionId = 0;
    }
   #endif
}

void MainComponent::setOutputLevelDb (double newLevelDb)
{
    appProperties.getUserSettings()->setValue ("outputLevelDb", newLevelDb);

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

    const auto description = "Marker " + juce::String (++markerCounter);
    const auto wallClockUtc = sessionStartUtc + juce::RelativeTime (e.elapsedSeconds);

    // Injected directly (message thread), never through the audio-thread FIFO.
    eventLog.addEvent (e, description, wallClockUtc);

    // Written while paused too: a marker dropped during a pause is exactly the
    // "re-patching here" note the pause needs.
    if (sessionLogger.isSessionActive())
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
    engine->getArtifactDetector().recaptureBaseline();

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

    // Both callbacks capture `this`: safe, because the dialog never outlives
    // this component (the dtor deletes it before anything else is torn down).
    const auto channelCountsNow = [this]
    {
        int outs = 0, ins = 0;
        getChannelCounts (outs, ins);
        return std::make_pair (outs, ins);
    };

    // Called when the dialog opens and again each time the device or channel
    // selection changes under it, so the rows always match the device above.
    const auto makeNamingSection = [this]() -> juce::Component*
    {
        refreshChannelConfig();   // size the name arrays to the current device

        int outs = 0, ins = 0;
        getChannelCounts (outs, ins);

        return new ChannelSetupPanel (outputChannelNames, inputChannelNames,
                                      referenceOutputForInput, inputMonitored,
                                      outs, ins,
                                      [this]
                                      {
                                          saveChannelConfig();
                                          refreshChannelConfig();
                                      });
    };

    juce::DialogWindow::LaunchOptions launch;
    launch.content.setOwned (new DeviceSetupPanel (engine->getDeviceManager(),
                                                   channelCountsNow, makeNamingSection));
    launch.dialogTitle = "Audio Device Setup";
    launch.dialogBackgroundColour = Palette::get (Palette::panelBackground);
    launch.escapeKeyTriggersCloseButton = true;
    launch.useNativeTitleBar = true;
    launch.resizable = true;

    deviceSetupWindow = launch.launchAsync();

    // Nothing is suppressed for having the dialog open: a change made in it is
    // recognised after the fact by trackDeviceSelection(), so a device that
    // genuinely fails while the dialog sits open is still reported.
}

void MainComponent::openSettings()
{
    if (settingsWindow != nullptr)
    {
        settingsWindow->toFront (true);
        return;
    }

    auto* content = new SettingsPanel (engine->getDetector(), engine->getArtifactDetector(), notificationEngine, appProperties);

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

    // Channel counts may have changed with the device.
    refreshChannelConfig();

    // A device-list change arrives as one of these too, among many broadcasts
    // that are not one (including those a failed recovery attempt causes). Only
    // a real change in what is listed lets recoverChosenDevice try again at
    // once and forget its backoff.
    juce::String listedNow;

    if (auto* type = engine->getDeviceManager().getCurrentDeviceTypeObject())
        listedNow = type->getDeviceNames (false).joinIntoString ("\n") + "\n>\n"
                      + type->getDeviceNames (true).joinIntoString ("\n");

    if (listedNow != deviceListSignature)
    {
        deviceListSignature   = listedNow;
        recoveryFailures      = 0;
        nextRecoveryAttemptMs = 0.0;
    }
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

    if (sessionLogger.isSessionActive())
        sessionLogger.logEvent (e, description);
}

juce::String MainComponent::applyDeviceSetup (const juce::AudioDeviceManager::AudioDeviceSetup& newSetup)
{
    // The device stops and restarts inside setAudioDeviceSetup(); bracket it so
    // that transition is not detected/logged as an audio fault.
    expectDeviceChangeFor (1000.0);
    const auto error = engine->getDeviceManager().setAudioDeviceSetup (newSetup, true);

    // Again, from now: the call above can take a while, and CoreAudio follows a
    // reconfiguration with a restart of its own about 100 ms afterwards.
    expectDeviceChangeFor (1000.0);

    // If the hardware refused this configuration the stop was swallowed by the
    // expected-change window — surface it now rather than pretending to run.
    // The user asked for this change and is at the controls, so here (and only
    // here) a refusal also stops the session.
    checkDeviceStillPresent (true);

    return error;
}

void MainComponent::checkDeviceStillPresent (bool mayStopSession)
{
    if (engine->getDeviceManager().getCurrentAudioDevice() != nullptr)
        return;

    const bool wasRunning = engine->isRunning();

    // Outside the user's own change this is a report about a running session,
    // made once per outage (reconcileStatus re-arms it when a device is open),
    // and not at all if the engine is holding for a loss: that one it reported.
    if (! mayStopSession
        && (! wasRunning || deviceGoneReported || engine->isOutputSilencedForDeviceLoss()))
        return;

    endExpectedDeviceChange();
    deviceGoneReported = true;

    logDeviceLostRow (mayStopSession ? "Audio device lost during reconfiguration"
                                     : "Audio device lost: no device is open");

    if (! mayStopSession)
        return;   // session, clock and log carry on; NO DEVICE status and the retry cover it

    if (wasRunning)
        stopAudio();

    statusBar.setAudioEngineState (TopStatusBar::EngineState::deviceError);
    statusBar.setAppStatus ("Device lost - check connections");
}

void MainComponent::logDeviceLostRow (const juce::String& description)
{
    Event e {};
    e.type           = EventType::AudioDeviceLost;
    e.severity       = Severity::Critical;
    e.channel        = ChannelId::None;
    e.elapsedSeconds = engine->getAnalysis().getElapsedSeconds();
    e.code           = eventCodeInstant;

    // Injected directly (message thread): the engine's own report of this stop
    // was suppressed or never made.
    eventLog.addEvent (e, description, sessionStartUtc + juce::RelativeTime (e.elapsedSeconds));

    if (sessionLogger.isSessionActive())
        sessionLogger.logEvent (e, description);

    maybeNotify (e, description);
}

void MainComponent::saveDeviceState()
{
    // The user's choice, not the manager's current state: after a device loss
    // the manager is on a fallback, and a tweak made there (a buffer size, say)
    // used to overwrite the saved interface for good.
    if (chosenDeviceState != nullptr)
        appProperties.getUserSettings()->setValue ("audioDeviceState", chosenDeviceState.get());
    else
        appProperties.getUserSettings()->removeValue ("audioDeviceState");

    appProperties.getUserSettings()->saveIfNeeded();
}

//==============================================================================
void MainComponent::setChosenDeviceState (std::unique_ptr<juce::XmlElement> state)
{
    chosenDeviceState = std::move (state);
    chosenOutputName.clear();
    chosenInputName.clear();

    if (chosenDeviceState != nullptr)
    {
        // Same reading as AudioDeviceManager's own: one combined name, or two.
        const auto combined = chosenDeviceState->getStringAttribute ("audioDeviceName");

        chosenOutputName = combined.isNotEmpty() ? combined
                                                 : chosenDeviceState->getStringAttribute ("audioOutputDeviceName");
        chosenInputName  = combined.isNotEmpty() ? combined
                                                 : chosenDeviceState->getStringAttribute ("audioInputDeviceName");
    }

    saveDeviceState();
}

juce::String MainComponent::describeDevices (const juce::String& outputName, const juce::String& inputName)
{
    if (inputName.isEmpty() || inputName == outputName)
        return outputName.isNotEmpty() ? outputName : juce::String ("no device");

    return outputName.isEmpty() ? inputName : outputName + " / " + inputName;
}

bool MainComponent::isOnUnchosenFallback() const
{
    return chosenDeviceState != nullptr
        && engine->getDeviceManager().getCurrentAudioDevice() != nullptr
        && (observedSetup.outputDeviceName != chosenOutputName
             || observedSetup.inputDeviceName != chosenInputName);
}

bool MainComponent::isChosenDeviceAvailable() const
{
    if (chosenDeviceState == nullptr)
        return false;

    auto* type = engine->getDeviceManager().getCurrentDeviceTypeObject();

    if (type == nullptr)
        return false;

    const auto listed = [type] (const juce::String& name, bool wantInputs)
    {
        return name.isEmpty() || type->getDeviceNames (wantInputs).contains (name);
    };

    return listed (chosenOutputName, false) && listed (chosenInputName, true);
}

bool MainComponent::isLiveDeviceAccepted() const
{
    return acceptedDeviceSet
        && observedSetup.outputDeviceName == acceptedOutputName
        && observedSetup.inputDeviceName == acceptedInputName;
}

void MainComponent::acceptLiveDevice()
{
    acceptedDeviceSet  = true;
    acceptedOutputName = observedSetup.outputDeviceName;
    acceptedInputName  = observedSetup.inputDeviceName;
}

bool MainComponent::isLiveDeviceWanted() const
{
    const bool liveIsChosen = chosenDeviceState != nullptr
                                && observedSetup.outputDeviceName == chosenOutputName
                                && observedSetup.inputDeviceName == chosenInputName;

    return liveIsChosen || isLiveDeviceAccepted();
}

void MainComponent::holdIfOnUnwantedDevice()
{
    if (! engine->isRunning()
        || engine->isOutputSilencedForDeviceLoss()                             // already silent
        || engine->getDeviceManager().getCurrentAudioDevice() == nullptr       // nothing to play through
        || isLiveDeviceWanted())
        return;

    // A device dropped where the engine was told to expect a stop (the second
    // after a user's change or after our own re-open), so it set no hold and
    // logged nothing — and JUCE has opened something else, usually the system
    // default. Silence first, then say so. Once: the next tick sees the hold.
    engine->holdOutputForDeviceLoss();

    logDeviceLostRow ("Audio moved to '" + describeDevices (observedSetup.outputDeviceName,
                                                             observedSetup.inputDeviceName)
                        + "', which was not selected; output held silent");
}

void MainComponent::expectDeviceChangeFor (double holdMs)
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();

    engine->setExpectedDeviceChange (true);
    expectedChangeUntilMs = nowMs + holdMs;

    // The stall watchdog must not count the gap a deliberate change leaves.
    lastMeasurementMs = nowMs;
}

void MainComponent::endExpectedDeviceChange()
{
    engine->setExpectedDeviceChange (false);
    expectedChangeUntilMs = 0.0;

    // ...nor judge the new device by time that passed while it was opening.
    lastMeasurementMs = juce::Time::getMillisecondCounterHiRes();
}

void MainComponent::trackDeviceSelection()
{
    const auto setupNow = engine->getDeviceManager().getAudioDeviceSetup();

    if (setupNow == observedSetup)
        return;

    // Were we, until this change, on a device the user had not chosen?
    const auto previousOutput = observedSetup.outputDeviceName;
    const auto previousInput  = observedSetup.inputDeviceName;
    const bool wasOnFallback  = chosenDeviceState != nullptr
                                  && (previousOutput != chosenOutputName || previousInput != chosenInputName);

    observedSetup = setupNow;

    // The manager's state XML is only rewritten by an explicit choice: the
    // selector in Audio Device Setup, or applyDeviceSetup. A fallback after a
    // device loss, and a device restarting by itself, leave it alone. So a
    // setup change WITH a state change is the user's doing; one without is not.
    auto managerState = engine->createDeviceStateXml();

    const bool userMadeChange = managerState == nullptr || observedManagerState == nullptr
                                  ? (managerState != nullptr) != (observedManagerState != nullptr)
                                  : ! managerState->isEquivalentTo (observedManagerState.get(), true);

    if (! userMadeChange)
        return;

    // The stop this caused is already in the queue as a device loss (the
    // selector applies its change itself, before anyone can be told to expect
    // it): drop those rows, and expect the restart CoreAudio adds on its own a
    // moment later.
    expectDeviceChangeFor (1000.0);
    swallowDeviceEventsUntilMs = juce::Time::getMillisecondCounterHiRes() + 1000.0;

    // Does this make the new device the user's choice? Always, unless they
    // only adjusted a fallback they never selected (same device names as the
    // fallback): that must not replace the saved interface.
    juce::String newOutput, newInput;

    if (managerState != nullptr)
    {
        const auto combined = managerState->getStringAttribute ("audioDeviceName");
        newOutput = combined.isNotEmpty() ? combined : managerState->getStringAttribute ("audioOutputDeviceName");
        newInput  = combined.isNotEmpty() ? combined : managerState->getStringAttribute ("audioInputDeviceName");
    }

    if (! wasOnFallback || newOutput != previousOutput || newInput != previousInput)
        setChosenDeviceState (managerState != nullptr ? std::make_unique<juce::XmlElement> (*managerState)
                                                      : nullptr);

    observedManagerState = std::move (managerState);

    // The engine held its output silent when the old device stopped. On the
    // device the user has now chosen (or already agreed to), let it go.
    if (isLiveDeviceWanted())
        engine->clearDeviceLossSilence();
}

void MainComponent::recoverChosenDevice (double nowMs)
{
    auto& deviceManager = engine->getDeviceManager();

    const bool running    = engine->isRunning();
    const bool active     = engine->isDeviceActive();
    const bool held       = engine->isOutputSilencedForDeviceLoss();
    const bool expecting  = expectedChangeUntilMs > 0.0;
    const bool noDevice   = deviceManager.getCurrentAudioDevice() == nullptr;
    const bool onFallback = isOnUnchosenFallback();

    // CoreAudio can stop a device and leave it listed and nominally current.
    // Nothing above notices: the device object exists and is the chosen one. So
    // time how long a running session has had an open device that is not running.
    if (running && ! noDevice && ! active && ! expecting)
    {
        if (deviceInactiveSinceMs <= 0.0)
            deviceInactiveSinceMs = nowMs;
    }
    else
    {
        deviceInactiveSinceMs = 0.0;
    }

    const bool stoppedDevice = deviceInactiveSinceMs > 0.0 && nowMs - deviceInactiveSinceMs > 3000.0;

    if (! noDevice && ! onFallback && ! stoppedDevice)
    {
        // Nothing to recover. The engine lets go of its device-loss silence by
        // itself only when the device that returns has the name of the one that
        // went; if it is still holding on a device that is wanted, it is
        // holding for nothing.
        if (running && active && held && isLiveDeviceWanted())
        {
            engine->clearDeviceLossSilence();
            logSessionNote ("Back on '" + describeDevices (observedSetup.outputDeviceName,
                                                           observedSetup.inputDeviceName)
                              + "'; output resumed", Severity::Info);
        }

        if (active)
            recoveryFailures = 0;

        return;
    }

    // Something is wrong. Not on top of a change already in progress, and not
    // before the backoff from the last failed attempt has run out.
    if (expecting || nowMs < nextRecoveryAttemptMs)
        return;

    const bool chosenAvailable = isChosenDeviceAvailable();

    enum class Action { none, reopenChosen, restartLast, resetStopped };
    auto action = Action::none;

    if (noDevice)
    {
        // Idle with an interface saved: the same thing launch does (the saved
        // interface, else whatever JUCE falls back to, which Start Audio will
        // ask about). Running: the saved interface if it is listed, otherwise
        // whatever was last open. Not reopening the last device first: after a
        // failed change that can be a setup the hardware refuses every time.
        if (chosenDeviceState != nullptr && (chosenAvailable || ! running))
            action = Action::reopenChosen;
        else if (observedSetup.outputDeviceName.isNotEmpty() || observedSetup.inputDeviceName.isNotEmpty())
            action = Action::restartLast;   // (the manager asserts if it has no device names)
    }
    else if (onFallback)
    {
        // A run the user knowingly started on this device is left alone — but
        // not while the engine is holding its output silent for a device loss:
        // then nothing is running on this device at all, and "left alone" would
        // mean silent for good.
        const bool leaveAlone = running && isLiveDeviceAccepted() && ! held;

        if (! leaveAlone && chosenAvailable)
            action = Action::reopenChosen;
    }
    else
    {
        action = Action::resetStopped;   // open, wanted, and not running
    }

    if (action == Action::none)
    {
        nextRecoveryAttemptMs = nowMs + 2000.0;   // only looked: look again soon
        return;
    }

    switch (action)
    {
        case Action::reopenChosen:
            // Our own change, so the fallback device stopping is expected — for
            // exactly as long as the call takes. The window is closed again
            // straight after it, not left open: a second drop in a lingering
            // window would set no hold and write no row.
            expectDeviceChangeFor (1000.0);

            if (running)
                deviceManager.initialise (2, 2, chosenDeviceState.get(), true);
            else
                engine->initialiseDevices (chosenDeviceState.get());   // also (re)attaches the idle metering callback

            endExpectedDeviceChange();
            break;

        case Action::restartLast:
            // Not bracketed: nothing is open to be stopped.
            deviceManager.restartLastAudioDevice();
            break;

        case Action::resetStopped:
            // restartLastAudioDevice() does nothing while a device object
            // exists, so close the dead one first (what the selector's own
            // "Reset Device" does). Not bracketed either: if it turns out to be
            // alive after all, stopping it should be on the record.
            deviceManager.closeAudioDevice();
            deviceManager.restartLastAudioDevice();
            deviceInactiveSinceMs = 0.0;
            break;

        case Action::none:
            break;
    }

    // These were our changes, not the user's: trackDeviceSelection must not
    // read them as a new selection.
    observedSetup        = deviceManager.getAudioDeviceSetup();
    observedManagerState = engine->createDeviceStateXml();

    // Open, actually running, and (if that was the aim) the saved interface.
    const bool succeeded = deviceManager.getCurrentAudioDevice() != nullptr
                             && engine->isDeviceActive()
                             && (action != Action::reopenChosen || ! isOnUnchosenFallback());

    if (succeeded)
    {
        recoveryFailures      = 0;
        nextRecoveryAttemptMs = nowMs + 2000.0;
    }
    else
    {
        // Each attempt stops and restarts whatever is open and gaps the
        // measurements, so a device that is listed but will not open is retried
        // less and less often. A change in the device list starts this over
        // (changeListenerCallback).
        static constexpr double backoffMs[] = { 2000.0, 5000.0, 15000.0, 60000.0 };

        nextRecoveryAttemptMs = nowMs + backoffMs[juce::jmin (recoveryFailures, 3)];
        ++recoveryFailures;

        // Left with nothing open: say so in the log (once). The session goes on.
        checkDeviceStillPresent (false);
    }
}

void MainComponent::reconcileStatus (double nowMs)
{
    // First, so that everything below describes the state it leaves: a running
    // session is never left playing through a device nobody chose.
    holdIfOnUnwantedDevice();

    const bool running      = engine->isRunning();
    const bool deviceActive = engine->isDeviceActive();
    const bool held         = engine->isOutputSilencedForDeviceLoss();
    const bool haveDevice   = engine->getDeviceManager().getCurrentAudioDevice() != nullptr;
    const bool onFallback   = isOnUnchosenFallback();
    const bool expecting    = expectedChangeUntilMs > 0.0;   // a change in progress, not a fault

    // Back on the saved interface: an earlier "play through the fallback" is
    // spent, and the next fallback is asked about afresh.
    if (chosenDeviceState != nullptr && haveDevice && ! onFallback)
        acceptedDeviceSet = false;

    if (haveDevice)
        deviceGoneReported = false;

    // 1. Transport and AUDIO cell follow the engine, not the last button press.
    transport.setAudioRunning (running);

    const auto chosen = describeDevices (chosenOutputName, chosenInputName);
    const auto live   = describeDevices (observedSetup.outputDeviceName, observedSetup.inputDeviceName);

    auto state = running ? TopStatusBar::EngineState::running : TopStatusBar::EngineState::stopped;
    juce::String alert;

    if (running && ! deviceActive && ! expecting)
    {
        state = TopStatusBar::EngineState::deviceLost;
        alert = "AUDIO DEVICE LOST: nothing is playing or being measured";
    }
    else if (running && held && ! expecting)
    {
        state = TopStatusBar::EngineState::outputHeld;
        alert = onFallback           ? "'" + chosen + "' LOST: output held silent (not playing through '" + live + "')"
              : isLiveDeviceWanted() ? juce::String ("DEVICE STOPPED: output held silent")
                                     : "Output held silent: '" + live + "' was not selected";
    }
    else if (! running && ! haveDevice)
    {
        state = TopStatusBar::EngineState::deviceError;
        alert = "No audio device is open";
    }

    statusBar.setAudioEngineState (state);

    // 2. LOG cell: a write error shows for exactly as long as the logger has
    //    one it has not recovered from, whatever Pause is doing.
    if (sessionLogger.isSessionActive())
    {
        const bool writeError = sessionLogger.hasUnrecoveredWriteError();

        statusBar.setLoggingState (writeError    ? TopStatusBar::LoggingState::writeError
                                   : loggingPaused ? TopStatusBar::LoggingState::paused
                                                   : TopStatusBar::LoggingState::active);

        if (writeError && alert.isEmpty())
            alert = "LOG WRITE ERROR: rows are being lost (retrying)";

        // Recovered: the next failure is a new one and gets its own alert.
        if (! writeError)
            writeFailureAlertShown = false;
    }

    // 3. The red alert text: the worst thing that is wrong right now, or nothing.
    if (alert.isEmpty() && stallActive)
        alert = "AUDIO STOPPED DELIVERING DATA";

    if (alert.isEmpty() && onFallback && ! (running && isLiveDeviceAccepted()))
        alert = "Saved interface '" + chosen + "' not found (now on '" + live + "')";

    statusBar.setAlert (alert);

    // 4. One alert box per device loss, and only for one that lasts: a restart
    //    that is over within a second or two is in the event log, not in a box.
    const bool deviceTrouble = running && ! expecting && (! deviceActive || held);

    if (! deviceTrouble)
    {
        deviceTroubleSinceMs = 0.0;
        deviceLossAlertShown = false;
    }
    else
    {
        if (deviceTroubleSinceMs <= 0.0)
            deviceTroubleSinceMs = nowMs;

        if (! deviceLossAlertShown && nowMs - deviceTroubleSinceMs >= 2000.0)
        {
            deviceLossAlertShown = true;

            const juce::String sessionGoesOn ("\n\nThe session clock and logging are still running, and "
                                              "the event log records when the device returns.");

            if (onFallback)
                showErrorAlert ("Audio interface lost",
                                "'" + chosen + "' stopped or was disconnected. macOS switched to '" + live
                                  + "', so the output is being held silent: pink noise is not played "
                                    "through it." + sessionGoesOn
                                  + "\n\nOutput resumes by itself when '" + chosen + "' is back. To use '"
                                  + live + "' instead, Stop Audio and Start Audio.");
            else
                showErrorAlert ("Audio device lost",
                                juce::String (deviceActive ? "The audio device stopped and the output is being held silent."
                                                           : "The audio device stopped. No device is running, so nothing "
                                                             "is playing or being measured.")
                                  + sessionGoesOn
                                  + "\n\nThe app keeps trying to reopen it. If it does not come back, check "
                                    "the interface and its cable, then Stop Audio and Start Audio.");
        }
    }
}

void MainComponent::checkDataFlow (double nowMs)
{
    // 1. Queue overflows: data the audio thread produced that never reached the
    //    message thread, so it is in neither CSV. One row per increase.
    const auto droppedEvents       = (juce::int64) engine->getDroppedEventCount();
    const auto droppedMeasurements = (juce::int64) engine->getDroppedMeasurementCount();

    if (droppedEvents > reportedDroppedEvents || droppedMeasurements > reportedDroppedMeasurements)
    {
        juce::StringArray lost;

        if (droppedMeasurements > reportedDroppedMeasurements)
            lost.add (juce::String (droppedMeasurements - reportedDroppedMeasurements) + " measurement row(s)");

        if (droppedEvents > reportedDroppedEvents)
            lost.add (juce::String (droppedEvents - reportedDroppedEvents) + " event(s)");

        logSessionNote ("Data lost before it could be logged: " + lost.joinIntoString (" and ")
                          + " (the app was not keeping up with the audio thread)", Severity::Warning);
    }

    reportedDroppedEvents       = droppedEvents;
    reportedDroppedMeasurements = droppedMeasurements;

    // 2. Stall: a running session that has stopped producing measurements is
    //    not testing anything, whatever the lamps say. drainMeasurementQueue()
    //    (which ran just before this) records every arrival and ends the stall.
    if (! engine->isRunning())
    {
        stallActive = false;
        return;
    }

    const double stallAfterMs = juce::jmax (3000.0,
                                            3000.0 * engine->getAnalysis().getMeasurementIntervalSeconds());

    if (! stallActive && nowMs - lastMeasurementMs > stallAfterMs)
    {
        stallActive = true;
        logSessionNote ("Audio stopped delivering data: no measurements for "
                          + juce::String (stallAfterMs / 1000.0, 0)
                          + " s (device stalled, asleep or gone)", Severity::Critical);
    }
}

void MainComponent::showStatusBarDeviceMenu (TopStatusBar::CellId cellId,
                                             juce::Rectangle<int> cellScreenBounds)
{
    auto& deviceManager = engine->getDeviceManager();
    const auto setup    = deviceManager.getAudioDeviceSetup();
    auto* currentDevice = deviceManager.getCurrentAudioDevice();

    constexpr int openFullSetupId    = 10000;
    constexpr int openChannelNamesId = 10001;

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

        case TopStatusBar::CellId::outChannels:
        case TopStatusBar::CellId::inChannels:
        {
            // Output and input counts are set independently: a rig may well feed
            // two outputs while capturing four returns.
            const bool forInputs = (cellId == TopStatusBar::CellId::inChannels);

            const int active = forInputs ? setup.inputChannels.countNumberOfSetBits()
                                         : setup.outputChannels.countNumberOfSetBits();

            int available = 2;
            if (auto* device = deviceManager.getCurrentAudioDevice())
                available = juce::jlimit (1, maxChannels,
                                          forInputs ? device->getInputChannelNames().size()
                                                    : device->getOutputChannelNames().size());

            for (int n = 1; n <= available; ++n)
            {
                juce::String label = juce::String (n) + " channel" + (n == 1 ? "" : "s");
                if (n == 1) label += " (mono)";
                else if (n == 2) label += " (stereo)";

                menu.addItem (n, label, true, n == active);
            }

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

    if (cellId == TopStatusBar::CellId::outChannels || cellId == TopStatusBar::CellId::inChannels)
        menu.addItem (openChannelNamesId, "Channel Names...");

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

        if (result == openChannelNamesId)
        {
            safeThis->openChannelSetup();
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

            case TopStatusBar::CellId::outChannels:
            case TopStatusBar::CellId::inChannels:
            {
                juce::BigInteger mask;
                mask.setRange (0, juce::jlimit (1, maxChannels, result), true);

                if (cellId == TopStatusBar::CellId::inChannels)
                {
                    newSetup.inputChannels           = mask;
                    newSetup.useDefaultInputChannels = false;
                }
                else
                {
                    newSetup.outputChannels           = mask;
                    newSetup.useDefaultOutputChannels = false;
                }
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

        // On success there is nothing to save here: trackDeviceSelection() sees
        // the new selection on the next tick and persists it as the user's choice.
        if (error.isNotEmpty())
            safeThis->statusBar.setAppStatus ("Device error: " + error);
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
    // From the device manager, so session_info.txt records the distinct input
    // and output devices rather than one combined AudioIODevice name.
    const auto deviceSetup = engine->getDeviceManager().getAudioDeviceSetup();
    info.inputDeviceName   = deviceSetup.inputDeviceName;
    info.outputDeviceName  = deviceSetup.outputDeviceName;

    // Channel identity travels with the session so the CSV columns, the events
    // file and session_info.txt all speak the user's names.
    int outs = 0, ins = 0;
    getChannelCounts (outs, ins);

    info.numOutputChannels = outs;
    info.numInputChannels  = ins;

    for (int ch = 0; ch < outs; ++ch)
        info.outputChannelNames.add (ch < outputChannelNames.size() ? outputChannelNames[ch]
                                                                     : "Out " + juce::String (ch + 1));

    for (int ch = 0; ch < ins; ++ch)
    {
        info.inputChannelNames.add (ch < inputChannelNames.size() ? inputChannelNames[ch]
                                                                   : "In " + juce::String (ch + 1));
        // What the analysis is actually comparing against on this device
        // (the stored mapping, fitted to the outputs that exist).
        info.referenceOutputForInput.add (juce::jlimit (0, juce::jmax (0, outs - 1),
                                                        ch < referenceOutputForInput.size() ? referenceOutputForInput[ch] : 0));
        info.inputMonitored.add (ch < inputMonitored.size() ? inputMonitored[ch] : true);
    }
    info.pinkNoiseSeed     = engine->getGenerator().getSeed();
    info.outputLevelDb     = engine->getGenerator().getLevelDb();

    return info;
}

void MainComponent::openLogFileSetup()
{
    chooseLogDestination (nullptr);
}

void MainComponent::toggleAudio()
{
    if (engine->isRunning())
        requestStopAudio();
    else
        startAudio();
}

void MainComponent::toggleLogging()
{
    if (sessionLogger.isSessionActive())
        requestStopLogging();
    else
        startLogging();
}

void MainComponent::addMarkerNow()
{
    if (sessionLogger.isSessionActive())
        addManualMarker();
}

void MainComponent::loadWaveformConfig()
{
    auto* settings = appProperties.getUserSettings();

    const int count = settings->getIntValue ("waveformLaneCount", 2);
    waveform.setNumLanes (count);

    const auto tokens = juce::StringArray::fromTokens (settings->getValue ("waveformLaneSources", {}), ",", {});

    for (int lane = 0; lane < tokens.size() && lane < WaveformDisplay::maxLanes; ++lane)
    {
        const auto token = tokens[lane].trim();
        WaveformDisplay::LaneSource src;

        if (token.startsWith ("out:"))
        {
            src.isInput = false;
            src.channel = token.fromFirstOccurrenceOf (":", false, false).getIntValue();
            src.active  = true;
        }
        else if (token.startsWith ("in:"))
        {
            src.isInput = true;
            src.channel = token.fromFirstOccurrenceOf (":", false, false).getIntValue();
            src.active  = true;
        }

        waveform.setLaneSource (lane, src);
    }
}

void MainComponent::saveWaveformConfig()
{
    juce::StringArray tokens;

    for (int lane = 0; lane < waveform.getNumLanes(); ++lane)
    {
        const auto src = waveform.getLaneSource (lane);
        tokens.add (! src.active ? "off"
                                 : (src.isInput ? "in:" : "out:") + juce::String (src.channel));
    }

    auto* settings = appProperties.getUserSettings();
    settings->setValue ("waveformLaneCount", waveform.getNumLanes());
    settings->setValue ("waveformLaneSources", tokens.joinIntoString (","));
    settings->saveIfNeeded();
}

void MainComponent::openChannelSetup()
{
    // Naming now lives inside the Audio Device Setup dialog, alongside the
    // device and channel-count controls it belongs with.
    openAudioDeviceSetup();
}

//==============================================================================
void MainComponent::getChannelCounts (int& numOutputs, int& numInputs) const
{
    const auto setup = engine->getDeviceManager().getAudioDeviceSetup();

    numOutputs = juce::jlimit (0, maxChannels, setup.outputChannels.countNumberOfSetBits());
    numInputs  = juce::jlimit (0, maxChannels, setup.inputChannels.countNumberOfSetBits());

    // Fall back to what the open device reports if the setup has no explicit mask.
    if (auto* device = engine->getDeviceManager().getCurrentAudioDevice())
    {
        if (numOutputs == 0)
            numOutputs = juce::jlimit (0, maxChannels, device->getActiveOutputChannels().countNumberOfSetBits());
        if (numInputs == 0)
            numInputs = juce::jlimit (0, maxChannels, device->getActiveInputChannels().countNumberOfSetBits());
    }
}

void MainComponent::refreshChannelConfig()
{
    int outs = 0, ins = 0;
    getChannelCounts (outs, ins);

    // Preserve names the user already set; only fill in what is missing.
    while (outputChannelNames.size() < outs)
        outputChannelNames.add ("Out " + juce::String (outputChannelNames.size() + 1));

    while (inputChannelNames.size() < ins)
        inputChannelNames.add ("In " + juce::String (inputChannelNames.size() + 1));

    // New inputs default to the output of the same number. Stored as that
    // (not squeezed into the current output count): the stored mapping is the
    // rig's, and must read the same whatever device happens to be open.
    while (referenceOutputForInput.size() < ins)
        referenceOutputForInput.add (referenceOutputForInput.size());

    while (inputMonitored.size() < ins)
        inputMonitored.add (true);

    // The mapping is fitted to the live device only where it is used, never in
    // the stored array. Clamping in place meant a spell on a 2-output fallback
    // rewrote a 4-output rig's [0,1,2,3] to [0,1,1,1], which then stayed (and
    // was saved by the next edit), with inputs 3 and 4 compared against the
    // wrong output for the rest of the run.
    for (int i = 0; i < ins; ++i)
    {
        engine->getAnalysis().setReferenceOutputForInput (i, juce::jlimit (0, juce::jmax (0, outs - 1),
                                                                             referenceOutputForInput[i]));
        engine->getAnalysis().setInputMonitored (i, inputMonitored[i]);
    }

    // Lane selectors offer every channel by its user-facing name.
    waveform.setAvailableChannels (outputChannelNames, inputChannelNames);

    refMeter.setNumChannels (juce::jmax (1, outs));
    refMeter.setChannelNames (outputChannelNames);

    inputMeter.setNumChannels (juce::jmax (1, ins));
    inputMeter.setChannelNames (inputChannelNames);
    inputMeter.setChannelMonitored (inputMonitored);

    eventLog.setChannelNames (outputChannelNames, inputChannelNames);

}

void MainComponent::loadChannelConfig()
{
    auto* settings = appProperties.getUserSettings();

    outputChannelNames.clear();
    inputChannelNames.clear();
    referenceOutputForInput.clear();

    inputMonitored.clear();
    for (const auto& token : juce::StringArray::fromTokens (settings->getValue ("inputMonitored", {}), ",", {}))
        if (token.trim().isNotEmpty())
            inputMonitored.add (token.trim() != "0");

    outputChannelNames.addTokens (settings->getValue ("outputChannelNames", {}), "\n", {});
    inputChannelNames.addTokens  (settings->getValue ("inputChannelNames",  {}), "\n", {});

    for (const auto& token : juce::StringArray::fromTokens (settings->getValue ("referenceOutputForInput", {}), ",", {}))
        if (token.trim().isNotEmpty())
            referenceOutputForInput.add (token.getIntValue());

    // A name left blank is saved as an empty line. Removing those lines moved
    // every later name up one channel at the next launch, mislabelling meters,
    // CSV columns and events; a blank keeps its slot and takes the default name.
    for (int i = 0; i < outputChannelNames.size(); ++i)
        if (outputChannelNames[i].trim().isEmpty())
            outputChannelNames.set (i, "Out " + juce::String (i + 1));

    for (int i = 0; i < inputChannelNames.size(); ++i)
        if (inputChannelNames[i].trim().isEmpty())
            inputChannelNames.set (i, "In " + juce::String (i + 1));
}

void MainComponent::saveChannelConfig()
{
    juce::StringArray refs;
    for (auto r : referenceOutputForInput)
        refs.add (juce::String (r));

    auto* settings = appProperties.getUserSettings();
    settings->setValue ("outputChannelNames", outputChannelNames.joinIntoString ("\n"));
    settings->setValue ("inputChannelNames",  inputChannelNames.joinIntoString ("\n"));
    settings->setValue ("referenceOutputForInput", refs.joinIntoString (","));

    juce::StringArray mon;
    for (auto m : inputMonitored)
        mon.add (m ? "1" : "0");
    settings->setValue ("inputMonitored", mon.joinIntoString (","));
    settings->saveIfNeeded();
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
