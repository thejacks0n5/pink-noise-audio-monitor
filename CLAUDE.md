# CLAUDE.md — Pink Noise Audio Monitor

Context for Claude Code (or any coding agent) picking up this project. Read this first.

## What this is

A professional audio testing/monitoring app for **macOS (Apple Silicon)**, written in
**C++20 + JUCE 8.0.4**. It generates deterministic pink noise out an output, captures the
returned signal on an input, aligns/compares them, and logs audio problems (dropouts,
clipping, level changes, …) for long overnight test sessions.

**Status:** V1 passed its defining test (a 60-hour run on a UMC404HD, July 2026:
no crash, flat memory, monotonic clock). Since then (October 2026) it has gained
in-app updates, artifact detection (clicks, noise, distortion, timing slips) and
a round of reliability fixes from two code reviews. `BUILD_NOTES.md` is the
operational record: §3b/§3c earlier fixes, §5 the overnight-run checklist, §6
releasing, §7 artifact detection and the October fixes. **§7 lists what has and
has not been verified on real hardware — read it before trusting a claim.**

**Target machine:** Mac mini, M4 Pro (Apple Silicon). Xcode + Core Audio + a real audio
interface (or a loopback device like BlackHole) are needed to actually run and test it.

## Scope discipline — IMPORTANT

Build **Version One only.** Do NOT start on advanced features until V1 compiles, runs
overnight, and is dependable. V1 = device selection, deterministic pink noise + level,
peak/RMS meters, basic waveform, start/stop logging, CSV measurement + event logs,
dropout/clip/level-change detection, live event log, basic macOS notification, stable
long-session operation. V1 has since grown multichannel support (per-channel analysis,
detection, meters, naming and CSV columns), a required named log destination, and
per-input Monitor toggles.

**Logging invariants worth preserving:** the session clock is monotonic (only
`AudioEngine::start()` zeroes it, with the callback detached); one unresolved fault produces
exactly one start/end pair and is NOT re-reported across suppression or device restarts;
unmonitored inputs are excluded from `measurements.csv` entirely.

Explicitly **out of scope for now** (later phases): FFT/spectrum, transfer function,
frequency-response/EQ/filter detection, compression/limiting detection, LUFS, session
reports, email/phone notifications, and the VST3 plugin. The architecture leaves room
for these, but don't build them yet. (Delay alignment is no longer out of scope: the
artifact detector does it.)

## Architecture

Namespace `pnam` throughout. Modules under `Source/`:

- `Model/DataModel.h(.cpp)` — `EventType` (incl. `AudioSettingsChanged`), `Severity`, `ChannelId`
  enums; the trivially-copyable `Measurement` and `Event` POD structs (the FIFO payloads);
  `SessionInfo` (message-thread only, carries channel names, reference mapping and Monitor flags).
  **Multichannel:** channel data lives in fixed arrays bounded by `pnam::maxChannels` (64) with
  runtime counts — the ceiling exists because the FIFO payload must stay a fixed-size POD.
  `Event::channelIndex` (not `channel`) is the authoritative channel identity.
  **The CSV schema is derived per-session from SessionInfo — it is no longer a fixed column list.**
- `Core/MeasurementQueue.h` — header-only lock-free SPSC queues (`juce::AbstractFifo`) for
  `Measurement` and `Event`. Audio thread pushes; message thread drains.
- `Core/PinkNoiseGenerator` — seeded xorshift64* + Paul Kellet economy pink filter. Deterministic:
  same seed ⇒ identical output. Level in dBFS (default −20), mute, fade in/out. Generates an
  INDEPENDENT stream per output channel (splitmix64-derived per-channel seeds, up to
  `pnam::maxChannels`), so a returning signal can be traced to the output feeding it.
- `Core/ReferenceHistory` — preallocated ring buffer of recent reference samples for comparison.
- `Core/AnalysisProcessor` — audio-thread per-block peak/RMS/gain-diff → emits `Measurement`s; exposes
  atomic getters the UI polls for live meters.
- `Core/EventDetector` — audio-thread threshold checks (dropout / clip / level change) with
  debounce + rate-limiting → emits `Event`s (start/end, not per-block).
- `Core/ArtifactDetector` — audio-thread comparison of each return against the exact reference
  that was sent. Per monitored input it finds the round-trip delay (GCC-PHAT cross-correlation,
  computed on a background thread from a snapshot the audio thread captures), fits a 32-tap NLMS
  model of the path's linear response (plus one integral term for AC coupling), and inspects the
  residual: `Click`, `Noise`, `Distortion`, `TimingSlip`, `SignalMismatch` events, and per-interval
  latency / residual / click-count figures for `measurements.csv`. Both signals are band-limited to
  ~200 Hz–16 kHz first, so **an artifact living only below ~200 Hz (pure mains hum, a slow thump)
  is not reliably reported**. Gain and polarity changes, dropouts and clock drift are deliberately
  NOT artifacts (the level detectors own those). `Tests/ArtifactDetectorTests.cpp` is its
  fault-injection rig: change the detector, run the rig.
- `Core/AudioEngine` — `juce::AudioDeviceManager` + IO callback: generator → output, capture input,
  feed artifact detection + analysis + level detection, push to queues. **No allocation or file I/O
  in the callback.** `isRunning()` is SESSION state (start() to stop()); `isDeviceActive()` is the
  device. After an unexpected device stop the output is held silent (so noise cannot come out of
  whatever JUCE falls back to) until the same device returns or the UI calls
  `clearDeviceLossSilence()`.
  Device lifecycle: `initialiseDevices (savedStateXml)` is called once from `MainComponent`'s ctor so
  a real device is open (and the user's saved choice restored) *before* Start Audio. `start()` only
  attaches the callback / reopens the last device — it must **never** call `initialiseWithDefaultDevices`,
  which would silently discard the user's Audio Device Setup selection. `createDeviceStateXml()`
  feeds persistence.
- `Logging/CsvWriter` + `Logging/SessionLogger` — message-thread CSV writers; per-session folder with
  `measurements.csv`, `events.csv`, `session_info.txt`.
- `Notifications/NotificationEngine` — rate-limited macOS desktop notifications.
- `Update/UpdateChecker` — in-app updates from GitHub releases. A background thread asks the
  GitHub API for the latest release, downloads its `*-macOS-universal.zip`, unpacks it with `ditto`
  and accepts it only if `codesign` confirms our Developer ID team + bundle id and the bundle's
  version is the newer one promised. The message thread then asks the user; installing is done by a
  detached shell script after the app quits. Checks run shortly after launch and daily (Release
  builds only), plus Check for Updates... in the application menu. **It never installs or quits on
  its own** — an update must not be able to end an overnight run. Release contract: `BUILD_NOTES.md` §6.
- `UI/*` — `DarkLookAndFeel`, `LevelMeterComponent`, `WaveformDisplay`, `EventLogComponent`,
  `TopStatusBar`, `TransportControls`, and `MainComponent` (the integration hub that owns everything).
  `TopStatusBar`'s OUT/IN/CH/RATE/BUFFER cells are click-to-edit: they fire `onCellClicked`, and
  `MainComponent::showStatusBarDeviceMenu` drops a `PopupMenu` that changes the device in place.
  `MainComponent` is a `juce::ChangeListener` on the `AudioDeviceManager` and persists the selection
  to `appProperties` ("audioDeviceState") on every change. `WaveformDisplay` draws up to
  `maxLanes` (8) stacked lanes, each with its own channel selector; `LevelMeterComponent` draws one
  bar per channel with its user-facing name. Channel naming, per-input Monitor toggles and the
  reference-output mapping live inside the Audio Device Setup dialog.
- `Main.cpp` — `juce::JUCEApplication` + `DocumentWindow` hosting `MainComponent`, plus the macOS main
  menu (`MenuBarModel` + `ApplicationCommandManager`). **Preferences lives in the application menu
  (Cmd-,), not on a transport button** — macOS renders it as "Settings…". `MainComponent::openSettings`
  and `openAudioDeviceSetup` are public so the menu can drive them.
- `UI/DelayMonitorComponent` — the DELAY strip under the transport: one cell per input showing the
  detector's round-trip latency minus a calibration offset (`ArtifactDetector::isAligned` /
  `getLatencyMs`, polled each tick). Calibrate with the output cabled straight to the input and the
  readout becomes the delay of whatever is then put in the path. `MainComponent` owns the offsets
  and saves them **per device configuration** (settings key `delayOffsetsMs|out|in|rate|buffer`),
  because an interface's own latency changes with sample rate and buffer size (UMC404HD at 48 kHz:
  22.93 ms at 512, 12.26 ms at 256). `measurements.csv` carries both `_latency_ms` (as measured)
  and `_delay_ms` (minus the offset); a calibration change mid-session is an Info row.
- Theme and icon: the palette is the `Palette` constants in `UI/DarkLookAndFeel.h` (blue accent
  `#3391FF`, matched to the icon; the reference trace and Info rows are deliberately NOT blue so
  they cannot be confused with it). The icon is a compiled asset catalog, `Resources/Assets.car`
  (source `Resources/Assets.xcassets`), bundled by CMake with `CFBundleIconName`; see the comment
  in `CMakeLists.txt` for why it is not `ICON_BIG` and how to rebuild it.

## The one rule that matters most (real-time safety)

The audio thread (`AudioEngine::audioDeviceIOCallbackWithContext` and everything it calls) must
**never** allocate, lock, block, write files, post notifications, or touch any `juce::Component`.
It may only do math and push PODs into the lock-free FIFOs. All file I/O, notifications, and UI
updates happen on the **message thread**. The FIFOs are drained there only: in
`MainComponent::timerCallback()`, and once at Start Audio / Start Logging so rows queued under the
previous session are not mistaken for the new one. Preserve this invariant in every change. If you need new
data on the UI, add a field to the POD and push it — do not reach across threads.

## Tests

```bash
cmake --build build-release --target pnam_tests -j && ./build-release/pnam_tests_artefacts/Release/pnam_tests
```

About 2.5 minutes in Release (do not run it in Debug). Exit code = failed checks. It runs the real
generator through a simulated path into the real `ArtifactDetector`, injects faults at known times
and checks each is reported once, on the right channel, at the right time, and that 80 minutes of
clean signal at five rate/buffer combinations reports nothing. A single test group can be run by
name (`clean`, `clicks`, `slips`, `noise`, `benign`, `mismatch`, `channels`, `delay`, `review`,
`floor`, `bench`).
The level detectors (`EventDetector`) and the logger have no in-repo tests yet.

## Conventions

- C++20, namespace `pnam`, `#pragma once`, JUCE-style naming.
- Header files are the **contract** between modules — they were designed first and implemented
  against. Prefer changing a `.cpp` to match a `.h`. Only change a header when the contract itself
  is genuinely wrong, and when you do, update every implementer + this file.
- Default output level is a conservative −20 dBFS; never surprise-blast the user with loud startup.

## Build

```bash
cmake -B build -G Xcode
cmake --build build --config Debug
# or open build/PinkNoiseAudioMonitor.xcodeproj in Xcode and build the app target
```

First configure git-clones JUCE (several hundred MB) via FetchContent — needs network. First
compile is slow (juce_dsp + juce_gui_extra). App bundle lands under `build/`. On first run macOS
prompts for microphone permission — grant it or the input path sees silence. For a real loopback
test you need output physically routed back to input, or an aggregate/loopback device (BlackHole).

See `BUILD_NOTES.md` §3–4 for known discrepancies (notifications need signing/entitlements to
actually display; settings dialog is intentionally minimal; waveform sampling is best-effort) and
the full first-build checklist. `ARCHITECTURE.md` and `UI_DESIGN.md` have the design rationale.

## Suggested session on a fresh machine (the overnight run)

1. **Build.** `brew install cmake` if missing. `cmake -B build -G "Unix Makefiles"
   -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j`. If the Xcode generator
   complains, the Xcode license is unaccepted — the Makefiles/CLT route is how
   every build so far was made. Expect zero errors and zero warnings; anything
   else is a regression, not a known issue.
2. **Configure for THIS machine's hardware** — settings do not travel between
   machines. Follow `BUILD_NOTES.md` §5 in order: device + channel counts,
   channel names, **Monitor unticked on unpatched inputs**, log destination,
   mic permission, sleep prevention.
3. **Smoke-test the loop**: Start Audio (Cmd-R), meters + waveform lanes live,
   Start Logging (Cmd-L), drop + rename a marker (Cmd-Shift-M, double-click the
   row), pull a cable to force a real dropout and confirm exactly ONE start
   event, then one end on reconnect.
4. **Overnight (the actual V1 acceptance test).** 12+ hours: no crash, flat
   memory, monotonic `elapsed_s`, no runaway log growth, one start/end pair per
   genuine fault, graceful recovery if the device hiccups.
5. Only after that passes: next spec phase (delay/correlation alignment, then
   FFT/frequency analysis).

A good opening prompt for Claude Code on that machine: *"Read CLAUDE.md and
BUILD_NOTES.md (especially §5), build the project, launch it, and help me
configure it for this machine's audio interface and an overnight run. Verify
each step on screen. Don't add features."*
