# Pink Noise Audio Monitor — V1 UI Design

> **This document is the original V1 interface design** and is kept as
> written below. What ships now differs as follows; `BUILD_NOTES.md` §10.7
> and §10.8 give the reasons and `docs/design/` the theme and icon sources.
>
> **Current state (October 2026, version 1.2.0):**
>
> - **Palette (§2).** The values in `Source/UI/DarkLookAndFeel.h` are the
>   truth; the table in §2 is the original. Surfaces `#0A0B0D` window,
>   `#15171B` / `#1A1C21` panels, `#23262D` / `#2B2E37` / `#363A45` controls,
>   outline `#353942`. Text `#E8EAEE` / `#A4AAB5` / `#6E7480`. **Accent blue
>   `#3391FF`** (dim `#3585E6`, bright and focus ring `#55A4FF`), matched to
>   the icon. Meters and severity: green `#3BD1A9`, amber `#FFD452`, red
>   `#FF6450`. Reference trace near-white `#CFD4E6`, input trace yellow-green
>   `#C7EB75`, Info rows cool grey `#BCC2CD`: deliberately not blue, so they
>   cannot be confused with the accent. Idle lamp `#555A65`.
> - **Layout (§3)** is now, top to bottom: status bar (44) → transport (56) →
>   **DELAY strip (58)** → meter column and waveform lanes → event log. The
>   meter column grows with the channel count instead of being a fixed
>   200 px, and the waveform is up to eight stacked lanes, each with its own
>   channel selector.
> - **Status bar:** no MODE cell. AUDIO also reads "NO DEVICE" or "Output
>   held"; LOG shows "Paused m:ss" and "Write error"; a red, bold alert line
>   replaces the right-hand status text when something is wrong. OUT, IN,
>   channel, rate and buffer cells are click-to-edit.
> - **Transport:** no Settings button (Settings is in the application menu,
>   Cmd-,). Stop Audio and Stop Logging ask first while a session is logged.
> - **Meters:** one bar per channel with its name; unmonitored inputs are
>   dimmed with "OFF".
> - **Event log:** a Note column; double-click to name a marker or add a note;
>   the type filter is built from the event-type enum.
> - **Dialogs** (§6 calls them modeless) are modal to the main window, though
>   audio, logging and the timer keep running behind them. Audio Device Setup
>   shows individual channels with naming, Monitor and reference rows that
>   follow the device, and a Done button.
> - **Pause** (§4, §6) gates measurements only; events and markers are still
>   written.
> - **The API manifest in §5** is the V1 one. Several classes have grown
>   (multichannel setters, lanes, status-bar alert, `DelayMonitorComponent`);
>   the headers are the contract.
> - **The app has an icon** (asset catalog in `Resources/`).

Namespace `pnam`. Headers under `Source/UI/`. Declarations only; bodies go in
matching `.cpp` files. Modules: `juce_gui_basics`, `juce_gui_extra`,
`juce_audio_utils`.

## 1. Design goals

An overnight test rig, not a music app: low-glare dark surfaces, zero
animation except meters/waveform, everything readable at a glance from across
a room. Green/amber/red is reserved exclusively for signal level and event
severity — never used decoratively.

## 2. Theme (DarkLookAndFeel.h)

Single source of truth is `pnam::Palette` (constexpr AARRGGBB + helpers
`get()`, `meterColourForDb()`, `severityColour()`).

| Token | Value | Use |
|---|---|---|
| windowBackground | `#0E1114` | app window |
| panelBackground / Alt | `#16191D` / `#1A1E23` | panels, alternating table rows |
| controlBackground / hover / pressed | `#1C2126` / `#242A31` / `#2B323A` | buttons, combos |
| outline / outlineFocused | `#262C33` / `#3C586E` | hairlines, focus |
| textPrimary / Secondary / Disabled | `#D7DBDF` / `#8B939B` / `#525A62` | text |
| accent / dim / bright | `#5B93B8` / `#3A5D75` / `#7FB3D3` | toggled-on buttons, slider fill, reference trace |
| meterGreen / Amber / Red | `#3FAE6A` / `#D9A53C` / `#D14B4B` | meter zones; amber ≥ −12 dBFS, red ≥ −3 dBFS |
| severityInfo / Warning / Critical | accent / amber / red | event rows, lamps |
| traceReference / InputL / InputR | accent / green / `#B87FD3` | waveform |

Custom draws: flat rounded buttons (radius 4), accent latched state, slim
slider with accent fill, dark table header/scrollbars/tooltips.
`DarkLookAndFeel::getMonoFont()` supplies the monospaced font every numeric
readout uses.

## 3. Screen layout (MainComponent::resized, default 1120×720, min 900×600)

```
┌──────────────────────────────────────────────────────────┐
│ TopStatusBar (44)                                        │
│ • AUDIO Running  • LOG Logging  02:14:37  Pink noise …   │
│   MacBook Pro Spk | Scarlett In | 2/2 | 48.0 kHz | 512   │
├──────────────────────────────────────────────────────────┤
│ TransportControls (56)                                   │
│ [Start][Stop] | [Log][Pause][Stop] | [Mute][level ─ -20] │
│ [Marker][Reset][Export]              [Audio Setup][Set…] │
├────────────┬─────────────────────────────────────────────┤
│ meters 200 │  WaveformDisplay (fills)                    │
│ REF | IN   │  ref (blue) vs input L/R (green/violet)     │
│  ██ | ██ █ │  click = freeze                             │
├────────────┴─────────────────────────────────────────────┤
│ EventLogComponent (30 % of remainder, clamp 140–280)     │
│ [sev ▾][type ▾][Follow][Clear]        123 events         │
│ 02:14:22.031  8062.0  Dropout  CRIT  Both  …             │
└──────────────────────────────────────────────────────────┘
```
8 px outer margin and gaps throughout.

## 4. Threading contract (unchanged from core)

Only `MainComponent::timerCallback` (one `juce::Timer`, 30 Hz) touches the
queues, SessionLogger, NotificationEngine, and UI state. Per tick: drain
measurement FIFO (→ CSV if logging & not paused, clip latch), drain event
FIFO (→ live log, CSV, Critical → notification), poll meter atomics,
pull waveform snapshots from the engine, refresh status bar. No UI class
ever touches the audio thread; `LevelMeterComponent` reads only the
`std::atomic` getters `AnalysisProcessor` publishes for exactly this purpose.

## 5. Public API manifest

### DarkLookAndFeel : juce::LookAndFeel_V4
- `DarkLookAndFeel()`; palette via `struct Palette` (see §2)
- `static juce::Font getMonoFont (float height)`
- overrides: button bg/text/toggle, linear slider (+thumb radius, textbox),
  combo, popup menu, table header, scrollbar, group outline, tooltip,
  text editor; `static constexpr float cornerRadius = 4.0f`

### LevelMeterComponent : juce::Component
- `enum class Role { reference, input }`; ctor `(Role, const String& title)`
- `void setSource (AnalysisProcessor*) noexcept` (not owned; nullptr detaches)
- `void setNumChannels (int)` / `int getNumChannels()`
- `void refresh()` — poll atomics + ballistics + repaint (call from the 30 Hz tick)
- `void updateValues (peakL, rmsL, peakR, rmsR, holdL, holdR)` (all dBFS)
- clip latch: `void notifyClipped (int channel)`, `void clearClipIndicator()`,
  `bool isClipIndicatorLit()`
- scale: `void setRangeDb (min,max)`, `getMinDb/getMaxDb`,
  `void setReadoutVisible (bool)`, `float proportionForDb (float)`
- `std::function<void()> onResetRequest` (click → clears clip, fires this;
  wire to `AnalysisProcessor::resetPeakHold`)
- constants: `defaultMinDb=-60`, `defaultMaxDb=0`, `defaultReleaseDbPerSecond=20`

### WaveformDisplay : juce::Component
- `enum class Trace { reference, inputL, inputR }`,
  `enum class AmplitudeScale { linear, decibels }`
- `void setSampleRate (double)`, `void setTimeWindowSeconds (double)` (+getter,
  default 2.0), `void setAmplitudeScale (AmplitudeScale)` (+getter),
  `void setDbFloor (float)` (default −60)
- `void setTraceVisible (Trace, bool)` / `bool isTraceVisible (Trace)`,
  `void setNumInputChannels (int)`
- `void pushSamples (Trace, const float*, int)` (message thread; decimates
  immediately, pointer not retained), `void clear()`
- `void setFrozen (bool)` / `bool isFrozen()`; click toggles freeze;
  `std::function<void(bool)> onFreezeChanged`
- `void refresh()` — repaint if dirty (call once per tick)

### EventLogComponent : juce::Component, TableListBoxModel (private)
- columns enum: `colTimestamp, colElapsed, colType, colSeverity, colChannel,
  colMeasured, colExpected, colDifference, colDescription`
- `void addEvent (const Event&, const String& description, juce::Time wallClockUtc)`
- `void clear()`; `int64 getTotalEventsReceived()`, `int getNumRowsStored()`
- filters: `void setMinimumSeverity (Severity)` (+getter),
  `void setTypeVisible (EventType,bool)` / `isTypeVisible` / `showAllTypes()`
- `void setAutoScrollEnabled (bool)` (+getter), `void setMaxStoredEvents (int)`
  (default 5000, display ring only — CSV keeps everything)
- `std::function<void()> onCleared`
- built-in toolbar: severity combo, type combo, Follow toggle, Clear, count label

### TopStatusBar : juce::Component
- `enum class EngineState { stopped, starting, running, deviceError }`
- `enum class LoggingState { inactive, active, paused, writeError }`
- `static constexpr int preferredHeight = 44`
- setters: `setAppStatus(String)`, `setAudioEngineState(EngineState)` (+getter),
  `setLoggingState(LoggingState)` (+getter), `setOutputDeviceName(String)`,
  `setInputDeviceName(String)`, `setChannelConfig(int out,int in)`,
  `setSampleRate(double Hz)`, `setBufferSize(int samples)`,
  `setSessionDuration(double s)` (negative = "—"), `setTestMode(String)`
- setters no-op on unchanged values and repaint only their own cell

### TransportControls : juce::Component
- `static constexpr int preferredHeight = 56`;
  slider range −60..0 dBFS, step 0.5
- callbacks (assign all): `onStartAudio, onStopAudio, onStartLogging,
  onStopLogging, onPauseLogging(bool), onMuteChanged(bool),
  onOutputLevelChanged(double dBFS), onResetSession, onAddMarker,
  onExportSession, onOpenSettings, onOpenDeviceSetup` — all `std::function`
- state reflection (never fires callbacks): `setAudioRunning(bool)`,
  `setLoggingActive(bool)`, `setLoggingPaused(bool)`, `setMuted(bool)`,
  `setSessionAvailable(bool)`, `setOutputLevelDb(double)` /
  `double getOutputLevelDb()`; bool getters for each
- enablement rules are internal (see header comment); widgets are private

### MainComponent : juce::Component, private juce::Timer
Members, declaration order = construction order:
```
DarkLookAndFeel                       lookAndFeel;
MeasurementQueue                      measurementQueue { defaultMeasurementQueueCapacity };
EventQueue                            eventQueue       { defaultEventQueueCapacity };
std::unique_ptr<AudioEngine>          engine;              // fwd-declared glue
SessionLogger                         sessionLogger;
NotificationEngine                    notificationEngine;
TopStatusBar                          statusBar;
TransportControls                     transport;
LevelMeterComponent                   refMeter, inputMeter;
WaveformDisplay                       waveform;
EventLogComponent                     eventLog;
SafePointer<DialogWindow>             deviceSetupWindow, settingsWindow;
SessionInfo currentSessionInfo; bool loggingPaused, sessionAvailable;
juce::Time sessionStartUtc; std::vector<float> waveformScratch;
juce::ApplicationProperties appProperties;
```
Private methods: `timerCallback(); drainMeasurementQueue();
drainEventQueue(); updateMetersAndWaveform(); updateStatusBar();
maybeNotify(const Event&, const String&); wireTransport(); startAudio();
stopAudio(); startLogging(); stopLogging(); setLoggingPaused(bool);
setOutputLevelDb(double); setOutputMuted(bool); addManualMarker();
resetSession(); exportSession(); openAudioDeviceSetup(); openSettings();
SessionInfo buildSessionInfo() const; juce::File getLogBaseFolder() const;`
Public: ctor/dtor, `paint`, `resized`.

**Expected AudioEngine surface** (glue header the integrator writes;
MainComponent.h documents it verbatim): ctor
`(MeasurementQueue&, EventQueue&)`; `start/stop/isRunning`;
`getDeviceManager()`; `getAnalysis()`, `getDetector()`, `getGenerator()`;
`getSampleRate/getBufferSize/getNumInputChannels/getNumOutputChannels`;
`getInputDeviceName/getOutputDeviceName`;
`readReferenceWaveform (float*, int)`, `readInputWaveform (int ch, float*, int)`.

## 6. Behaviours worth calling out

- **Start/Stop Audio vs Logging** are fully independent; queues drain and the
  live UI updates whenever audio runs, CSV writes happen only while a session
  is active and not paused.
- **Pause Logging** gates CSV writes only; live log/meters keep moving.
- **Manual Marker** is injected on the message thread directly (never via the
  audio FIFO): it appears in the live log and events.csv with
  `EventType::ManualMarker`.
- **Clip latch** on the input meter is fed from drained Measurements
  (`clipCountBlock > 0`) and Clipping events; click to clear (also resets
  peak hold via `onResetRequest`).
- **Device setup / settings** are modeless `DialogWindow`s
  (`AudioDeviceSelectorComponent`; threshold fields map 1:1 to
  `EventDetector::Thresholds`) tracked with `SafePointer`.
- **Notifications**: only `Severity::Critical` events reach
  `NotificationEngine::postCritical`; all rate limiting lives in the engine.
