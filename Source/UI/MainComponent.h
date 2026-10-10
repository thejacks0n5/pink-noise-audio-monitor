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
      timerCallback() is where the FIFOs are drained, every tick. The event
      FIFO is additionally drained by startAudio() and beginLoggingSession(),
      which need the live log up to date at that instant. All of it is on the
      message thread, which is also the only writer of SessionLogger /
      NotificationEngine / UI state. Per tick:
        1. measurementQueue.popAll:
             - latest values → meters handled separately via atomics (below)
             - clipCountBlock > 0            → inputMeter.notifyClipped(...)
             - if logging active && !paused  → sessionLogger.logMeasurement(m)
        2. eventQueue.popAll:
             - desc = SessionLogger::composeDescription (e)
             - eventLog.addEvent (e, desc, startUtc + e.elapsedSeconds)
             - if logging active             → sessionLogger.logEvent (e, desc)
               (pause holds back measurement rows only, never events)
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

#include <functional>
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

    /** Menu-driven transport (Main.cpp binds these to keyboard shortcuts).
        Stopping asks first while a logging session is active. */
    void toggleAudio();
    void toggleLogging();
    void addMarkerNow();

    /** Real engine / logger state, for the menu's item names and enablement. */
    bool isAudioRunning() const;
    bool isLoggingActive() const;

    /** Fired (message thread) whenever either of the two states above changes,
        whatever caused it. Main.cpp uses it to refresh the menu bar. */
    std::function<void()> onTransportStateChanged;

    /** Runs `proceed` straight away when no logging session is active. While
        one is, asks first (async, "Keep Logging" / "<actionName>") and runs it
        only if the user clicks the action: Stop Logging, Stop Audio and Quit
        each end an unattended run, and were one stray click or keypress away.
        Return and Escape both keep logging.
        @param actionName   button text and title, e.g. "Stop Logging".
        @param consequence  one sentence saying what the action does to the log. */
    void confirmIfLogging (const juce::String& actionName,
                           const juce::String& consequence,
                           std::function<void()> proceed);

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
    void startLogging();                     // prompt-if-needed, pre-flight, then beginLoggingSession
    void beginLoggingSession();              // buildSessionInfo() → sessionLogger.startSession
    void stopLogging();
    void setLoggingPaused (bool shouldPause);// gates measurement rows only; events are
                                             // still written, and the pause itself is logged

    // What the buttons and menu call: the two above, behind confirmIfLogging().
    // stopAudio()/stopLogging() themselves never ask (shutdown, device loss).
    void requestStopAudio();
    void requestStopLogging();

    /** Checks, at Start Logging, the things that waste an unattended run: output
        muted, a monitored input at digital silence, a fault already active, a
        log folder that is unwritable or nearly full. Starts the session at once
        when none apply; otherwise lists the ones that do and asks. */
    void runLoggingPreflight();
    juce::StringArray collectPreflightProblems();

    /** One live-log row, as needed to write it into events.csv after the fact. */
    struct LiveLogRow
    {
        Event event;
        juce::String description, note;
    };

    /** Conditions that started during the current audio session and have not
        ended: every start row in the live log with no matching end row. */
    std::vector<LiveLogRow> findActiveFaultRows();

    /** Injects an EventType::Other row (message thread, same path as a manual
        marker): live log always, events.csv while a session is active. */
    void logSessionNote (const juce::String& description, Severity severity);

    /** One async alert for a failure the user must not miss. The status bar
        still gets its text; this is for when nobody is reading the status bar. */
    void showErrorAlert (const juce::String& title, const juce::String& message);

    /** Keeps the Mac from idle-sleeping for as long as a logging session is
        active (system sleep stops the audio device and with it the run). */
    void holdSleepAssertion();
    void releaseSleepAssertion();
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

    /** Writes the device the user CHOSE (chosenDeviceState) to appProperties
        under "audioDeviceState" — not whatever the manager is running on now,
        which after a device loss is a fallback nobody asked for. */
    void saveDeviceState();

    /** Compares the live device setup against lastKnownSetup and, if anything
        differs, logs one AudioSettingsChanged event naming the change (event log
        always; CSV whenever a session is active). */
    void logAudioSettingsChange();

    /** Applies a device setup with the engine bracketed by
        setExpectedDeviceChange(), so the stop/restart it causes is not logged as
        an AudioDeviceLost fault. @returns the device error, empty on success. */
    juce::String applyDeviceSetup (const juce::AudioDeviceManager::AudioDeviceSetup&);

    /** The expected-change window suppresses audioDeviceStopped, so a device
        that genuinely died during it would go unreported. Called after every
        applyDeviceSetup and whenever a window closes: if no device is open,
        writes one AudioDeviceLost row.
        @param mayStopSession  true only on the user's own applyDeviceSetup
               path, where a refused setting also stops audio and logging (the
               user is at the controls). Everywhere else — a window lapsing, the
               automatic recovery — the session is left running: one failed
               re-open must not end an unattended run. */
    void checkDeviceStillPresent (bool mayStopSession);

    /** Injects one Critical AudioDeviceLost row (live log, events.csv while a
        session is active, notification). */
    void logDeviceLostRow (const juce::String& description);

    //==========================================================================
    // Which device the user chose, and what to do when we are not on it.

    /** Tells the engine that device stops are expected from now until holdMs
        from now. Used around a change this component makes itself, and for the
        second after any user-made change: CoreAudio follows a reconfiguration
        with a restart of its own ~100 ms later. Lapses by itself in
        timerCallback — it is no longer held for as long as the Audio Device
        Setup dialog is open, which hid real device losses all night. */
    void expectDeviceChangeFor (double holdMs);

    /** Closes the window now. */
    void endExpectedDeviceChange();

    /** Per tick, before the event queue is drained: notices that the manager's
        explicit (user-made) device state has changed, which is the one exact
        sign that a device stop was the user's doing — the selector component
        applies its changes itself, so they cannot be bracketed in advance. */
    void trackDeviceSelection();

    /** The live device is not the one the user chose (and they did choose one). */
    bool isOnUnchosenFallback() const;
    bool isChosenDeviceAvailable() const;

    /** The user agreed to play through the live device: by answering the
        Start Audio prompt, or by starting on it when no interface was saved.
        Held as device NAMES, so agreeing to one fallback never covers the next. */
    bool isLiveDeviceAccepted() const;
    void acceptLiveDevice();

    /** The live device is the chosen one or an accepted one. */
    bool isLiveDeviceWanted() const;

    /** The invariant, enforced every tick: a running session never plays
        through a device that is neither chosen nor accepted. The engine holds
        its output silent by itself after a stop it did not expect, but a device
        that drops inside an expected-change window is not reported to it — so
        if we find ourselves on such a device with the output not held, hold it
        here and write the row the engine did not. */
    void holdIfOnUnwantedDevice();

    /** "UMC404HD", or "UMC404HD / Built-in Mic" when output and input differ. */
    static juce::String describeDevices (const juce::String& outputName, const juce::String& inputName);

    /** Adopts a device state as the user's choice and persists it. */
    void setChosenDeviceState (std::unique_ptr<juce::XmlElement> state);

    /** While something is wrong (no device, a device that has stopped running,
        or a fallback nobody chose): reopens the last device, or re-applies the
        chosen setup once that device is listed again, and releases the engine's
        device-loss silence when we are back on a wanted device. Attempts that
        fail back off (2, 5, 15, 60 s) until one succeeds or the device list
        changes; each re-open gaps the measurements, so it must not be hammered. */
    void recoverChosenDevice (double nowMs);

    /** Per tick: transport buttons, AUDIO and LOG cells and the red alert text
        follow what the engine and logger are actually doing. */
    void reconcileStatus (double nowMs);

    /** Per tick: reports queue overflows (data lost before it reached the log)
        and a session that has stopped producing measurements. */
    void checkDataFlow (double nowMs);

    // The user's own selection, as persisted. May be null (never chose).
    std::unique_ptr<juce::XmlElement> chosenDeviceState;
    juce::String chosenOutputName, chosenInputName;

    // The manager's explicit state and live setup as last seen by
    // trackDeviceSelection(), to notice changes to either.
    std::unique_ptr<juce::XmlElement> observedManagerState;
    juce::AudioDeviceManager::AudioDeviceSetup observedSetup;

    // The device the user agreed to play through (see isLiveDeviceAccepted).
    bool acceptedDeviceSet = false;
    juce::String acceptedOutputName, acceptedInputName;

    // All getMillisecondCounterHiRes() values; 0 = not set.
    double expectedChangeUntilMs      = 0.0;   // engine's expected-change flag lapses then
    double swallowDeviceEventsUntilMs = 0.0;   // device-lost rows from a user's own change
    double nextRecoveryAttemptMs      = 0.0;   // recoverChosenDevice does nothing before this
    double deviceInactiveSinceMs      = 0.0;   // session running, device open but not running
    double deviceTroubleSinceMs       = 0.0;   // running session, device not doing its job
    int    recoveryFailures           = 0;     // consecutive failed re-opens (the backoff step)
    bool   deviceLossAlertShown       = false;
    bool   deviceGoneReported         = false; // "no device is open" row written for this outage

    // Output + input device names as last listed, to tell a real device-list
    // change from the manager's many other change broadcasts.
    juce::String deviceListSignature;

    // Data-flow watchdog.
    juce::int64 reportedDroppedEvents = 0, reportedDroppedMeasurements = 0;
    double      lastMeasurementMs     = 0.0;   // hi-res counter, for the 3 s test
    juce::Time  lastMeasurementWall;           // wall clock, for the gap length
    bool        stallActive           = false;

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

    // Async alert box. Owned here so it cannot outlive this component:
    // destroying a ScopedMessageBox closes its box without calling a callback.
    juce::ScopedMessageBox errorBox;

    /** One async question with a safe answer and an action. The safe button is
        the default: it is first, takes Return AND Escape, and is what every
        other way of dismissing the box amounts to. The action runs only when
        its own button is clicked. Separate from errorBox, so an error never
        replaces a pending question; a second question replaces the first. */
    void showConfirm (juce::MessageBoxIconType icon,
                      const juce::String& title, const juce::String& message,
                      const juce::String& safeButtonText, const juce::String& actionButtonText,
                      std::function<void()> onAction);

    // The open question, if any. Self-deleting when answered; deleted in the
    // dtor otherwise (its callback then sees the safe answer and a dead
    // SafePointer, and does nothing).
    juce::Component::SafePointer<juce::AlertWindow> confirmWindow;

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
    juce::uint32 pauseStartedMs = 0;         // getMillisecondCounter() at the last pause
    int   markerCounter        = 0;          // "Marker N" labels, reset per session
    bool  sessionAvailable     = false;      // a session folder exists to export
    juce::Time sessionStartUtc;              // for event wall-clock timestamps

    // eventLog.getTotalEventsReceived() at the last Start Audio: live-log rows
    // from here on share the current session clock, earlier ones do not.
    juce::int64 audioSessionFirstEvent = 0;

    // Logging failures: startSession reports its reason through onLoggingError
    // before returning false, so the reason is collected for the one alert the
    // failed start raises; a failure outside startSession is a mid-run write
    // error, alerted once per session.
    bool startingLoggingSession  = false;
    juce::String loggingStartError;
    bool writeFailureAlertShown  = false;

    // Last states reported through onTransportStateChanged.
    bool reportedAudioRunning  = false;
    bool reportedLoggingActive = false;

    // IOPMAssertionID of the sleep assertion held while logging; 0 = none.
    juce::uint32 sleepAssertionId = 0;

    // Scratch buffer reused by updateMetersAndWaveform() for engine waveform
    // reads (preallocated once; never allocated per tick)
    std::vector<float> waveformScratch;

    // Persistent user prefs (level, seed, thresholds, log folder, window size)
    juce::ApplicationProperties appProperties;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace pnam
