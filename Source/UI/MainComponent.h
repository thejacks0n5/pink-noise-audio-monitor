#pragma once
/*
  ==============================================================================
    MainComponent.h
    Pink Noise Audio Monitor (pnam) — top-level component and message-thread hub.

    OWNERSHIP (constructed in this order, destroyed in reverse):
      1. DarkLookAndFeel        — installed as default L&F
      2. MeasurementQueue + EventQueue (the two audio→message SPSC FIFOs)
      3. AudioEngine            — glue class (Source/Core/AudioEngine.h, written
                                  by the integration engineer per ARCHITECTURE.md
                                  §5). Constructed with references to both
                                  queues; owns AudioDeviceManager,
                                  PinkNoiseGenerator, ReferenceHistory,
                                  AnalysisProcessor, EventDetector.
      4. SessionLogger, NotificationEngine
      5. UI children: TopStatusBar, TransportControls, LevelMeterComponent x2,
         WaveformDisplay, EventLogComponent

    Expected AudioEngine surface (documented here so MainComponent.cpp and
    AudioEngine.h are written against the same contract):
        AudioEngine (MeasurementQueue&, EventQueue&);
        bool start(); void stop();                 // fade-out before stop
        bool isRunning() const;
        juce::AudioDeviceManager& getDeviceManager();
        AnalysisProcessor&  getAnalysis();         // for meter atomics
        EventDetector&      getDetector();         // thresholds / recapture
        PinkNoiseGenerator& getGenerator();        // level / mute / seed
        double getSampleRate() const; int getBufferSize() const;
        int getNumInputChannels() const; int getNumOutputChannels() const;
        juce::String getInputDeviceName() const, getOutputDeviceName() const;
        // message-thread waveform snapshots (benign-race reads of the
        // reference ring + an equivalent input ring the engine maintains):
        int readReferenceWaveform (int channel, float* dest, int maxSamples);
        int readInputWaveform (int channel, float* dest, int maxSamples);

    TIMER (single juce::Timer, ~30 Hz — startTimerHz (30) in the ctor):
      timerCallback() is the ONLY place FIFOs are drained and the only writer
      of SessionLogger / NotificationEngine / UI state. Per tick:
        1. measurementQueue.popAll:
             - latest values → meters handled separately via atomics (below)
             - clipCountBlock > 0            → inputMeter.notifyClipped(...)
             - if logging active && !paused  → sessionLogger.logMeasurement(m)
        2. eventQueue.popAll:
             - desc = SessionLogger::composeDescription (e)
             - eventLog.addEvent (e, desc, startUtc + e.elapsedSeconds)
             - if logging active && !paused  → sessionLogger.logEvent (e, desc)
             - if e.severity == Critical     → notificationEngine.postCritical
        3. refMeter.refresh(); inputMeter.refresh();      // poll atomics
        4. engine.readReferenceWaveform / readInputWaveform →
           waveform.pushSamples(...); waveform.refresh();
        5. statusBar.setSessionDuration (analysis.getElapsedSeconds()); plus
           any cheap status text updates.
      NEVER touches the audio thread. NEVER blocks (CSV writes are buffered
      by SessionLogger's flush interval).

    LAYOUT — resized() (window default 1120 x 720, minimum 900 x 600):
      ┌──────────────────────────────────────────────────────────────┐
      │ TopStatusBar                                  (h = 44)       │
      ├──────────────────────────────────────────────────────────────┤
      │ TransportControls                             (h = 56)       │
      ├───────────────┬──────────────────────────────────────────────┤
      │ meterPanel    │  WaveformDisplay                             │
      │ (w = 200)     │  (remaining width)                           │
      │ ┌───────────┐ │                                              │
      │ │ refMeter  │ │   central area = remaining height minus      │
      │ │ (left ½)  │ │   event log                                  │
      │ │ inputMeter│ │                                              │
      │ │ (right ½) │ │                                              │
      │ └───────────┘ │                                              │
      ├───────────────┴──────────────────────────────────────────────┤
      │ EventLogComponent                     (h = 30% of remaining, │
      │  (docked bottom, full width)           clamped 140..280 px)  │
      └──────────────────────────────────────────────────────────────┘
      8 px outer margin, 8 px gaps. The meter column width scales with the
      channel count (capped at half the window) and is split between the two
      meters in proportion to their bar counts; each meter draws one bar per
      channel with its user-facing name beneath. The waveform draws up to
      WaveformDisplay::maxLanes stacked lanes, each with its own channel selector.

    All bodies live in MainComponent.cpp (integration engineer).
  ==============================================================================
*/

#include <memory>
#include <vector>
#include <juce_data_structures/juce_data_structures.h>  // juce::ApplicationProperties
                                                         // (INTEGRATOR NOTE: added — the
                                                         // fixed contract used this type
                                                         // by value without the include or
                                                         // the module being linked; see
                                                         // BUILD_NOTES.md "Header discrepancies")
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>   // juce::AudioDeviceSelectorComponent

#include "../Model/DataModel.h"
#include "../Core/MeasurementQueue.h"
#include "../Logging/SessionLogger.h"
#include "../Notifications/NotificationEngine.h"

#include "DarkLookAndFeel.h"
#include "TopStatusBar.h"
#include "TransportControls.h"
#include "LevelMeterComponent.h"
#include "WaveformDisplay.h"
#include "EventLogComponent.h"

namespace pnam
{

class AudioEngine;   // glue class — Source/Core/AudioEngine.h (ARCHITECTURE.md §5)

//==============================================================================
class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener   // juce::AudioDeviceManager
{
public:
    //==========================================================================
    MainComponent();
    ~MainComponent() override;   // stops timer, stops logging, stops engine,
                                 // clears default LookAndFeel

    //==========================================================================
    // juce::Component
    void paint (juce::Graphics&) override;   // fills Palette::windowBackground
    void resized() override;                 // layout per the diagram above

    //==========================================================================
    // Public so the macOS main menu (Main.cpp) can drive them. Preferences lives
    // in the application menu (Cmd-,), not on a button in the transport bar.
    void openAudioDeviceSetup();             // modeless DialogWindow hosting a
                                             // juce::AudioDeviceSelectorComponent
                                             // bound to engine.getDeviceManager()
    void openSettings();                     // modeless DialogWindow: thresholds
                                             // (EventDetector::Thresholds),
                                             // notification toggles, seed,
                                             // log base folder picker

    /** Native chooser for the log name + folder. Logging cannot start until this
        has been set once; Start Logging opens it automatically if it hasn't. */
    void openLogFileSetup();

    /** Dialog for naming every input/output channel and choosing which output
        each input is referenced against. Names label the meters, the CSV
        columns and the events file. */
    void openChannelSetup();

    /** Menu-driven transport (Main.cpp binds these to keyboard shortcuts). */
    void toggleAudio();
    void toggleLogging();
    void addMarkerNow();

    /** Exposed so the window can persist its bounds alongside the app state. */
    juce::PropertiesFile* getUserSettings() { return appProperties.getUserSettings(); }

private:
    //==========================================================================
    // juce::Timer — the single ~30 Hz message-thread pump (see header block)
    void timerCallback() override;

    // timerCallback helpers (all message thread)
    void drainMeasurementQueue();
    void drainEventQueue();
    void updateMetersAndWaveform();
    void updateStatusBar();
    /** Critical-event → macOS notification, honouring NotificationEngine's
        rate limiting. */
    void maybeNotify (const Event&, const juce::String& description);

    //==========================================================================
    // Transport action handlers (assigned to TransportControls callbacks in
    // wireTransport(); each reflects resulting state back into the UI)
    void wireTransport();

    void startAudio();                       // engine.start(); statusBar/buttons
    void stopAudio();                        // stops logging first if active
    void startLogging();                     // prompt-if-needed, then beginLoggingSession
    void beginLoggingSession();              // buildSessionInfo() → sessionLogger.startSession
    void stopLogging();
    void setLoggingPaused (bool shouldPause);// gates CSV writes; queues still drain
    void setOutputLevelDb (double newLevelDb);   // generator.setLevelDb + SessionInfo
    void setOutputMuted (bool shouldMute);       // generator.setMuted + status bits
    void addManualMarker();                  // pushes an EventType::ManualMarker
                                             // Event directly into the message-
                                             // thread path (no FIFO needed)
    void resetSession();                     // clears meters/waveform/event log,
                                             // resetPeakHold, recaptureBaseline
    void exportSession();                    // reveals session folder in Finder /
                                             // juce::FileChooser copy-to destination

    //==========================================================================
    // Audio device selection (message thread)

    /** juce::ChangeListener on the AudioDeviceManager: persists the device
        selection whenever it changes, so a choice survives Start/Stop Audio and
        app restarts. */
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    /** Writes the manager's state to appProperties under "audioDeviceState". */
    void saveDeviceState();

    /** Compares the live device setup against lastKnownSetup and, if anything
        differs, logs one AudioSettingsChanged event naming the change (event log
        always; CSV when logging is active and not paused). */
    void logAudioSettingsChange();

    /** Applies a device setup with the engine bracketed by
        setExpectedDeviceChange(), so the stop/restart it causes is not logged as
        an AudioDeviceLost fault. @returns the device error, empty on success. */
    juce::String applyDeviceSetup (const juce::AudioDeviceManager::AudioDeviceSetup&);

    /** The expected-change window suppresses audioDeviceStopped, so a device
        that genuinely died during it (unplugged mid-dialog, or a setup the
        hardware refused) would leave the app claiming to run forever. Called
        whenever the window closes and after every apply: if no device is open,
        reports the loss and reflects the stopped state everywhere. */
    void checkDeviceStillPresent();

    bool wasExpectingDeviceChange = false;

    /** Last setup we reported on, so changes can be described as before → after. */
    juce::AudioDeviceManager::AudioDeviceSetup lastKnownSetup;

    //==========================================================================
    // Per-channel configuration (message thread)

    /** Live channel counts from the device manager, clamped to maxChannels. */
    void getChannelCounts (int& numOutputs, int& numInputs) const;

    /** Grows/shrinks the name and reference arrays to the current channel count,
        filling gaps with "Out n" / "In n" and identity reference mapping, then
        pushes counts/names into the meters and the mapping into the engine.
        Cheap and idempotent — called whenever the device configuration changes. */
    void refreshChannelConfig();

    void loadChannelConfig();
    void saveChannelConfig();

    /** Waveform lane layout, persisted like the channel names. */
    void loadWaveformConfig();
    void saveWaveformConfig();

    juce::StringArray outputChannelNames, inputChannelNames;
    juce::Array<int>  referenceOutputForInput;

    /** Per-input detection enable. Unticked inputs still meter, but are excluded
        from dropout/clip/level-change detection AND from measurements.csv — an
        unpatched input would otherwise report a permanent critical dropout. */
    juce::Array<bool> inputMonitored;


    /** Drops a PopupMenu under a clicked TopStatusBar cell letting the user
        change output/input device, channel mode, sample rate or buffer size in
        place; each menu also offers the full Audio Device Setup dialog. */
    void showStatusBarDeviceMenu (TopStatusBar::CellId, juce::Rectangle<int> cellScreenBounds);

    //==========================================================================
    // Session helpers
    /** Fills a SessionInfo from live engine state + user prefs (seed, level,
        base log folder, device names, channel counts, new sessionId). */
    SessionInfo buildSessionInfo() const;
    /** Where session folders are created; persisted in appProperties.
        Default: ~/Documents/PinkNoiseAudioMonitor/. */
    juce::File getLogBaseFolder() const;

    /** User-chosen label for this rig's logs; each session folder is named
        "<baseName>_<timestamp>". Empty until the user picks one. */
    juce::String getLogBaseName() const;

    /** @returns true once a log name and folder have been chosen. */
    bool isLogDestinationConfigured() const;

    /** Opens the native chooser and invokes onChosen(true) if the user picked a
        destination. Async — the chooser must outlive the call, hence the member
        below. */
    void chooseLogDestination (std::function<void (bool)> onChosen);

    // Kept alive across the async FileChooser callback.
    std::unique_ptr<juce::FileChooser> logFileChooser;

    //==========================================================================
    // 1. Theme (constructed first, destroyed last)
    DarkLookAndFeel lookAndFeel;

    // 2. Audio→message FIFOs (owned here; engine gets references — the queues
    //    must outlive the engine, hence declared before it)
    MeasurementQueue measurementQueue { defaultMeasurementQueueCapacity };
    EventQueue       eventQueue       { defaultEventQueueCapacity };

    // 3. Engine (heap: forward-declared type; created in the ctor)
    std::unique_ptr<AudioEngine> engine;

    // 4. Message-thread services
    SessionLogger      sessionLogger;      // onLoggingError → event log + status
    NotificationEngine notificationEngine;

    // 5. UI children
    TopStatusBar       statusBar;
    TransportControls  transport;
    LevelMeterComponent refMeter   { LevelMeterComponent::Role::reference, "REF OUT" };
    LevelMeterComponent inputMeter { LevelMeterComponent::Role::input,     "INPUT" };
    WaveformDisplay    waveform;
    EventLogComponent  eventLog;

    // Modeless windows (SafePointer: user can close them directly)
    juce::Component::SafePointer<juce::DialogWindow> deviceSetupWindow;
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow;

    //==========================================================================
    // Session / UI state (message thread only)
    SessionInfo currentSessionInfo;          // valid while logging or exportable
    bool  loggingPaused        = false;
    int   markerCounter        = 0;          // "Marker N" labels, reset per session
    bool  sessionAvailable     = false;      // a session folder exists to export
    juce::Time sessionStartUtc;              // for event wall-clock timestamps

    // Scratch buffer reused by updateMetersAndWaveform() for engine waveform
    // reads (preallocated once; never allocated per tick)
    std::vector<float> waveformScratch;

    // Persistent user prefs (level, seed, thresholds, log folder, window size)
    juce::ApplicationProperties appProperties;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace pnam
