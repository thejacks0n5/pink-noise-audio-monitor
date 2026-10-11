# CLAUDE.md — Pink Noise Audio Monitor

Context for Claude Code (or any coding agent) picking up this project. Read this first.

## What this is

A professional audio testing/monitoring app for **macOS 12+** (universal: Apple Silicon and
Intel), written in **C++20 + JUCE 8.0.4**. It generates deterministic pink noise out an output,
captures the returned signal on an input, aligns and compares them, shows the round-trip delay
live, and logs audio problems (dropouts, clipping, level changes, clicks, noise, distortion,
timing slips, a return that is not the signal) for long unattended sessions.

**Status (October 2026): version 1.2.0 is released.** Public repo
`github.com/thejacks0n5/pink-noise-audio-monitor`; signed, notarized downloads on its Releases
page; installed copies from 1.0.2 on update themselves from the latest release.

- 1.0.0 passed V1's defining test (a 60-hour run on a UMC404HD, July 2026).
- 1.0.2 added in-app updates. 1.1.0 added artifact detection, the fixes from two rounds of code
  review, a blue theme and an app icon. 1.2.0 added the live delay monitor with calibration.
- **Nothing newer than 1.0.0 has run for longer than about twelve minutes on hardware, and no
  real fault or unplug has ever been put through it.** Read `BUILD_NOTES.md` §12 before trusting
  a claim.

**`BUILD_NOTES.md` is the record.** §1–5 the original V1 notes (annotated where superseded),
§6 releasing, §7 artifact detection, §8 delay monitor, **§9 everything done and when, §10 every
decision and why, §11 every review finding and its outcome, §12 open items / known limits / not
verified, §13 reference tables** (events, CSV columns, defaults, settings keys, shortcuts).
`docs/design/` holds the icon and theme sources and reasoning.

**Development machine:** Mac mini, M4 Pro, with a Behringer UMC404HD (Out 1 cabled to In 1).
Its measured figures are in `BUILD_NOTES.md` §12.4.

## Scope discipline — IMPORTANT

V1 was built under "Version One only, no extra features". Everything since (updater, artifact
detection, delay monitor, theme and icon) was added because the owner asked for it. **The rule
now: build what the owner asks for and nothing else.** No unrequested features, refactors or
"while I was there" changes.

**Logging invariants worth preserving:** the session clock is monotonic (only
`AudioEngine::start()` zeroes it, with the callback detached, and it refuses while a session is
running); `timestamp_utc` never goes backwards; one unresolved fault produces exactly one
start/end pair and is NOT re-reported across suppression or device restarts; a Start is never
lost to rate limiting; unmonitored inputs are excluded from `measurements.csv` entirely; event
type numbers are append-only.

**Safety invariants:** pink noise must never come out of a device the user did not choose (an
unexpected device stop holds the output silent); nothing may stop a logging session except the
user (dialogs ask, with the safe button as default; the updater never installs by itself);
default output level is −20 dBFS.

Still **out of scope** until asked for: FFT/spectrum, transfer function,
frequency-response/EQ/filter detection, compression/limiting detection, LUFS, session reports,
email/phone notifications, the VST3 plugin. Offered but not built: a "delay moved from
calibration" event (`BUILD_NOTES.md` §12.1).

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
- `Core/EventDetector` — audio-thread level checks with debounce → emits `Event`s (start/end, not
  per-block). **Dropout is relative:** the input 20 dB (`dropoutRelativeDb`) below the level the
  reference and the learned baseline predict, with capped exit hysteresis; the absolute
  `dropoutThresholdDb` only applies before a baseline exists. Level change is judged on 400 ms
  smoothed energies. A Start is retried until queued; an End is never rate-limited. The reasons
  are in `BUILD_NOTES.md` §10.3 — read them before changing a threshold rule.
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

Threads: the **audio thread** (device callback); the **message thread** (UI, logging,
notifications, all device management); a low-priority **alignment thread** owned by `AudioEngine`
that runs the artifact detector's cross-correlations (it polls; the audio thread never wakes it);
and the updater's own background thread for network and signature checks.

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
cmake -B build-release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build-release -j
open "build-release/PinkNoiseAudioMonitor_artefacts/Release/Pink Noise Audio Monitor.app"
```

This is how every build has been made (Command Line Tools + CMake from Homebrew; the Xcode
generator also works if the Xcode license is accepted). Without the architectures flag the build
is arm64 only. First configure git-clones JUCE via FetchContent and needs network; if a build
folder's cached JUCE location has gone, delete the folder and configure again. Expect zero
warnings from anything under `Source/` (JUCE's own bundled code prints some).

Things that bite:

- **Launch the bundle with `open`, not by running the binary from a shell.** A shell child
  inherits the shell's microphone access and hides entitlement problems.
- **To try the app without touching real settings,** launch with the environment variable
  `CFFIXED_USER_HOME` set to an empty folder
  (`open -n --env CFFIXED_USER_HOME=/some/empty/dir <app>`).
- A hardened-runtime (Developer ID) build needs the `com.apple.security.device.audio-input`
  entitlement or every input reads −inf with no prompt. CMake sets it; sign with the generated
  entitlements file (`BUILD_NOTES.md` §6).
- The deployment target line must stay above `project()` in `CMakeLists.txt`.
- **Publishing a release ships it** to every installed copy. Run the tests, sign, notarize,
  write the release notes as plain text, then prove it by letting the previous release update
  itself (`BUILD_NOTES.md` §6, §10.2).

## How work is done here

- Verify before claiming: build it, run it, look at it on screen, and say plainly whether a thing
  was checked by simulation, on the real interface, or only by reading the code.
- Before playing through the interface, confirm the loop quietly (start at −40 dBFS and check the
  return) rather than assuming it is patched.
- Non-trivial changes to detection, devices or logging get an independent review by someone who
  did not write them; both rounds so far found real defects (`BUILD_NOTES.md` §11).
- Hardware finds what simulation does not (two designs changed because of it). Prefer a few
  minutes on the real interface to another simulated case.
- When something changes behaviour, update `BUILD_NOTES.md` (§10 for the decision, §12 for
  anything left open) and this file in the same change.

## Where things stand, and what would be worth doing next

Nothing is in progress. Open, in rough order of value (detail in `BUILD_NOTES.md` §12):

1. **A long run of the current version,** with a cable pull and an interface unplug during it:
   the only long run on record is 1.0.0, and device-loss recovery has never met a real unplug.
2. **A real device of known delay** through the delay monitor.
3. Decide on the offered "delay moved from calibration" event.
4. Bring the level-detector and logger test harnesses into `Tests/` (the reviewers' were
   throwaway and are gone).
5. The smaller items in §12.1, §12.2 and §12.5.
