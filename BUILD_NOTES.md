# Build Notes — Pink Noise Audio Monitor (pnam) V1

Integration pass: implemented `Source/UI/MainComponent.cpp` against the fixed
`MainComponent.h` contract, and did a consistency pass across every other
`Source/*.cpp` file. This document summarizes what's implemented, what was
fixed, what's stubbed, and a first-build checklist for Apple Silicon.

## 1. What's implemented for V1

- **MainComponent.cpp** constructs and owns: `AudioEngine` (audio-thread
  glue — device I/O, generator, analysis, detection), `SessionLogger`,
  `NotificationEngine`, and all six UI children (`TopStatusBar`,
  `TransportControls`, two `LevelMeterComponent`s, `WaveformDisplay`,
  `EventLogComponent`), applying `DarkLookAndFeel` as the default L&F.
- `resized()` implements the documented layout: status bar (44px) → transport
  (56px) → [200px meter column (ref | input) | waveform, remainder] →
  event log docked bottom (30% of remainder, clamped 140–280px), 8px
  margins/gaps throughout.
- `timerCallback()` (30 Hz, the *only* place either FIFO is drained) does, in
  order: drain `MeasurementQueue` (meter clip latch + CSV row if logging &
  not paused), drain `EventQueue` (live event log always, CSV row if logging
  & not paused, rate-limited notification if `Severity::Critical`), poll
  meter atomics via `refresh()`, pull reference/input waveform snapshots from
  the engine into one preallocated scratch buffer and push them into
  `WaveformDisplay`, then refresh the status bar (session duration, device
  names, channel/rate/buffer info — all change-detecting setters, so this is
  cheap even every tick).
- Every `TransportControls` callback is wired: Start/Stop Audio
  (`AudioEngine::start/stop`, independent of logging), Start/Stop/Pause
  Logging (`SessionLogger`), Mute and Output Level (→
  `PinkNoiseGenerator`), Reset Session (clears meters/waveform/event log,
  `resetPeakHold()`, `recaptureBaseline()`), Add Manual Marker (an `Event`
  built and injected directly into the event log + CSV on the message
  thread — bypasses the audio-thread FIFO, per contract), Export Session
  (reveals the session folder in Finder), Open Audio Device Setup (a
  `juce::AudioDeviceSelectorComponent` in a modeless `DialogWindow` bound to
  `engine->getDeviceManager()`), Open Settings (a small modeless dialog:
  dropout threshold, level-change tolerance, min-dropout hold, notification
  enable/cooldown, pink-noise seed [persisted, applied at next Start Audio —
  `setSeed()` is non-real-time], recapture-baseline button, log-folder
  chooser — all backed by `juce::ApplicationProperties`).
- `SessionInfo::startTimeUtc` is anchored to the moment audio actually
  started (not to when logging starts), and `sessionId` is a fresh
  wall-clock label generated at `Start Logging` time — this means multiple
  Start/Stop Logging cycles within one Start Audio session get distinct,
  non-colliding session folders while `elapsedSeconds → UTC` conversion in
  the CSVs stays correct throughout.

**Audio thread does no file I/O**: confirmed by inspection.
`AudioEngine::audioDeviceIOCallbackWithContext` only calls
`PinkNoiseGenerator::processBlock`, `ReferenceHistory::write`,
`AnalysisProcessor::processBlock` (which only does math + `queue.push()`,
itself a lock-free, non-blocking, non-allocating `juce::AbstractFifo`
operation), and `EventDetector::processBlockStats` (same FIFO push). None of
`CsvWriter`, `SessionLogger`, `NotificationEngine`, or any `juce::Component`
is touched from that callback or from anything it calls. All CSV/notify work
happens exclusively inside `MainComponent::timerCallback()` on the message
thread.

## 2. Consistency-pass fixes (bugs found in existing files, fixed in place)

These were genuine compile/link blockers found while cross-checking every
`.cpp` against its header, fixed conservatively (no header signatures
changed):

1. **`Source/Notifications/NotificationEngine.cpp`** — `postCritical()`
   returned unqualified `unavailable` / `suppressedCooldown` /
   `suppressedGlobal` / `posted`. `RateLimitResult` is an `enum class`, so
   these don't compile without the `RateLimitResult::` qualifier. Fixed all
   four `return` statements. Also fixed
   `notification.identifier = "pnam_critical_" + juce::String (type)` —
   `type` is `EventType` (a scoped enum with no implicit conversion to a
   `juce::String`-constructible numeric type); changed to
   `juce::String ((int) type)`.
2. **`Source/Model/DataModel.cpp` was missing entirely.** `DataModel.h`
   declares `toString(EventType|Severity|ChannelId)` "impl in .cpp", and
   `SessionLogger.cpp` / `EventLogComponent.cpp` both call these — with no
   `.cpp` providing them, the link would fail. Created
   `Source/Model/DataModel.cpp` implementing all three exactly per the
   header's documented return strings (e.g. `ChannelId` → `"L"/"R"/"Both"/"-"`).
3. **`Source/UI/MainComponent.h` uses `juce::ApplicationProperties`
   by value** (`juce::ApplicationProperties appProperties;`), which lives in
   the `juce_data_structures` module. That module was neither included nor
   linked anywhere (`CMakeLists.txt` only listed core/audio/gui/dsp modules).
   This is a genuine gap in the fixed contract, not something a `.cpp` fix
   can paper over (a value member needs a complete type wherever the header
   is included, including `Main.cpp`). Fixed minimally:
   - Added `#include <juce_data_structures/juce_data_structures.h>` to the
     top of `MainComponent.h` (one line, no signatures touched, clearly
     commented as an integrator note).
   - Added `juce::juce_data_structures` to `target_link_libraries` in
     `CMakeLists.txt`.
   - Also added `JUCE_PUSH_NOTIFICATIONS=1` to `target_compile_definitions`
     — `NotificationEngine`'s doc comment says this define is required for
     `juce::PushNotifications` to actually do anything (see §3 below).

No other header was modified. All other fixes were pure `.cpp` edits.

## 3. Known discrepancies / stubs a human should check first

- **Desktop notifications on macOS need real entitlements.**
  `NotificationEngine` degrades silently (returns
  `RateLimitResult::unavailable`) if notifications aren't authorized — this
  works as designed — but to get real banners on a signed macOS build you'll
  need: `JUCE_PUSH_NOTIFICATIONS=1` (now set), the "User Notifications"
  capability/entitlement, and — because `HARDENED_RUNTIME_ENABLED` is on —
  a valid code-signing identity (ad-hoc signing may not be enough for local
  notifications to actually display). Treat this as **untested on real
  hardware**; the code path is implemented per contract but only a real
  signed build on a real Mac will confirm delivery.
- **Settings dialog is intentionally minimal** (per the task's "stub dialog
  ok" allowance): it exposes dropout threshold, level-change tolerance,
  min-dropout hold, notification enable/cooldown, pink-noise seed, and a log
  folder picker. It does **not** expose `clipThresholdLinear`, `clipFreeMs`,
  `criticalClipMs`, `levelChangeHoldMs`, `baselineCaptureMs`, `settlingMs`,
  `maxEventsPerSecond`, or `NotificationEngine::setMaxPerMinute` — all of
  these keep their contract defaults. Extending the panel is straightforward
  (same pattern as the exposed fields) if V1.1 needs it.
- **Waveform sampling is a fixed-size, best-effort per-tick window** (`sampleRate
  / 30`, capped at the 8192-sample preallocated scratch buffer), not a
  sample-accurate "exactly the new samples since last tick" read. This
  matches `ReferenceHistory::readLatest`'s "benign race" contract (message
  thread reading data written by the audio thread) but means the waveform
  can very slightly overlap or gap frame-to-frame under UI thread jitter —
  acceptable for a monitoring display, not for offline analysis (the CSVs
  are the source of truth for that).
- **Clip-indicator channel attribution is approximate for `Measurement`-driven
  latches.** `Measurement::clipCountBlock` is an aggregate count with no
  per-channel breakdown, so `MainComponent` lights both L and R clip lamps
  whenever it's nonzero; only `EventType::Clipping` events (which carry a
  `ChannelId`) get precise per-channel lamps.
- **`AudioEngine.h`/`.cpp` already existed** (not written by me) and matches
  the "Expected AudioEngine surface" documented in `MainComponent.h`
  verbatim — I read it carefully and treated it as authoritative alongside
  the fixed contract; no changes were needed there.
- **`CsvWriter.h` uses `std::unique_ptr` without including `<memory>`
  directly** — almost certainly fine in practice (`juce_core` pulls in
  `<memory>` transitively everywhere else in this codebase), but flagging it
  as a low-risk item; if a future JUCE version changes its internal include
  graph, add `#include <memory>` there.

## 3b. Device-selection + UI fixes (second session, verified on hardware)

Found by using the app on a real interface; all four were reported by the user.

1. **"You pick a device, close the window, and nothing changes."** Two causes:
   `AudioEngine::start()` called `deviceManager.initialiseWithDefaultDevices (2, 2)`
   on every start, which *reset the selection to the system defaults*; and the
   ctor never initialised the device manager at all, so the selector had no live
   device to edit before Start Audio. Now `initialiseDevices()` runs once at
   construction (restoring saved state) and `start()` only attaches the callback
   / calls `restartLastAudioDevice()`. Selection is persisted to
   `appProperties` under "audioDeviceState" via a `ChangeListener`, so it
   survives Start/Stop **and** app restarts. Verified: the settings file records
   `UMC404HD 192k` with `audioDeviceInChans="1"`, and it is restored on relaunch.
2. **No mono option.** `AudioDeviceSelectorComponent` was constructed with
   `showChannelsAsStereoPairs = true`, so channels could only be toggled in
   pairs. Now `false`, and the channel range starts at 1 — the dialog shows
   individual `Out 1..4` / `In 1..4` checkboxes and mono works.
3. **No way to accept.** The selector applies changes live (there is no apply
   step), but there was no way to confirm/dismiss. Wrapped in `DeviceSetupPanel`
   with an explicit **Done** button.
4. **Settings was a button.** Moved to the macOS application menu as
   Preferences — macOS displays it as "Settings…" with Cmd-, — via a
   `MenuBarModel` + `ApplicationCommandManager` in `Main.cpp`. An **Audio** menu
   holds "Audio Device Setup..." (Cmd-Shift-A). The transport button is gone.
5. **New: click-to-edit status bar.** OUT/IN/CH/RATE/BUFFER cells show a
   chevron, highlight on hover, and open a `PopupMenu` to change the setting in
   place; each menu also offers the full dialog. The bar now sources device
   identity from the `AudioDeviceManager` rather than the engine's audio-thread
   atomics, so it is populated *before* Start Audio and distinguishes the input
   from the output device (`AudioIODevice::getName()` returns one combined name
   for both — `session_info.txt` was recording the same string twice).

Two layout overflows were found while checking the above on screen:
`TopStatusBar`'s fixed cell widths summed to 1130 px in a 1120 px window (the
app-status cell collapsed and BUFFER was clipped) — cells now shrink
proportionally with per-cell minimums; and `TransportControls` laid out the
right-aligned Audio Setup button *last*, so it collapsed to "A..." — the
right-hand buttons are now claimed before the left-to-right run.

## 3c. Log integrity across audio-settings changes (verified on hardware)

1. **CSV timestamps ran backwards.** `AnalysisProcessor::prepare()` zeroed
   `samplesProcessed`, and it runs on *every* device restart — so `elapsed_s`,
   and the `timestamp_utc` derived from it, jumped back to zero mid-session
   whenever the device was reconfigured. A real log had rows dated 16:19
   followed by rows dated 16:11. The clock now carries forward: `prepare()`
   folds the previous configuration's elapsed time into `elapsedOffsetSeconds`
   *before* adopting the new sample rate (so a rate change converts correctly),
   and only the new `resetSessionClock()` — called from `AudioEngine::start()`,
   with the callback detached — returns it to zero. Verified: 182 rows across
   three reconfigurations, zero backwards jumps.
2. **Settings changes are now logged.** New `EventType::AudioSettingsChanged`
   (value 7, appended so existing numeric values are unchanged).
   `MainComponent::logAudioSettingsChange()` diffs the live
   `AudioDeviceSetup` against `lastKnownSetup` and injects one `Info` event
   naming what moved, e.g. *"Audio settings changed: channels 1 out / 1 in ->
   2 out / 2 in"*. Message-thread injection, same path as a manual marker;
   written to `events.csv` when logging is active and not paused. The manager
   broadcasts for other reasons too, so nothing is logged when the diff is empty.
3. **Reconfiguration no longer registers as a fault.** `audioDeviceStopped()`
   used to push an `AudioDeviceLost` warning and clear `running` on every
   user-initiated change — a real events.csv had six of them from settings
   changes alone, and the cleared flag would have left the session marked
   stopped. `AudioEngine::setExpectedDeviceChange()` now brackets user-driven
   changes (`MainComponent::applyDeviceSetup` for the status-bar menus; held for
   the lifetime of the Audio Device Setup dialog). Detection itself was already
   safe here — `audioDeviceAboutToStart` re-arms the 3 s `settling` window and
   `EventDetector::prepare()` re-requests the level baseline — so no dropout or
   level-change false positives were produced either.

   **Tradeoff to be aware of:** while the flag is set, a *genuine* device
   failure is also suppressed. That window is the duration of an explicit user
   change, or however long the Audio Device Setup dialog stays open.
   `audioDeviceError` is deliberately not gated.

**Observed once, not fully diagnosed:** switching mono → stereo on a UMC404HD at
192 kHz made CoreAudio renegotiate three times (visible as the three events
above) and the session ended up stopped, needing a manual Start Audio. It
recovered fully and came back in stereo. This may be the interface refusing an
intermediate configuration; it could not be conclusively separated from the
synthetic mouse input used to drive the test. Worth re-checking by hand before
relying on mid-session channel changes during a long run.

## 4. First build checklist (Apple Silicon macOS)

1. **Toolchain**: Xcode 15+ (for a modern-enough SDK/Clang for C++20 +
   `JUCE_STRICT_REFCOUNTEDPOINTER`), CMake ≥ 3.22 (already pinned in
   `CMakeLists.txt`), `cmake -B build -G Xcode`.
2. **JUCE version pin**: `CMakeLists.txt` fetches JUCE `8.0.4` via
   `FetchContent` — first configure will `git clone` JUCE (several hundred
   MB); make sure the build machine has network access, or pre-seed a local
   clone and point `FetchContent_Declare` at it (`SOURCE_DIR`) if building
   offline/CI.
3. **Microphone permission**: `MICROPHONE_PERMISSION_ENABLED TRUE` is set;
   on first run macOS will prompt — grant it, or input-channel analysis will
   silently see silence (`inputInactive` status bit will be set, which is
   handled gracefully by `EventDetector`'s suppression logic, but you won't
   see anything interesting on the input meter/waveform without it).
4. **Notifications**: see §3 above — expect `unavailable` posts on an
   unsigned/dev build; this is by design, not a bug, but don't be surprised
   when Notification Center stays quiet during local testing.
5. **Audio device**: the app opens the user's saved device at construction
   (`AudioEngine::initialiseDevices`), and attaches the callback immediately with
   output silenced so input meters are live before Start Audio. `start()` never
   selects defaults — doing so used to discard the user's selection. For a genuine loopback/monitoring test you need
   an aggregate device or hardware loopback (e.g. BlackHole, or physically
   routing the output back into the input) so the "reference vs. returned
   input" comparison actually measures something non-trivial.
6. **Hardened runtime + code signing**: `HARDENED_RUNTIME_ENABLED TRUE` is
   set in `CMakeLists.txt`; for anything beyond local `Debug` runs (TestFlight,
   distribution, or reliable notification delivery) you'll need a real
   Developer ID / signing identity configured in Xcode, not just the
   CMake-generated project's default ad-hoc signing.
7. **First compile will be slow**: JUCE's `juce_dsp` + `juce_gui_extra` +
   FetchContent clone add real time to a clean configure/build; expect the
   first `cmake --build` to take several minutes even on Apple Silicon.
8. **Log folder default**: `~/Documents/PinkNoiseAudioMonitor/` unless
   overridden via Settings → "Choose Log Folder..." (persisted in
   `~/Library/Application Support/PinkNoiseAudioMonitor/PinkNoiseAudioMonitor.settings`
   via `juce::ApplicationProperties`).

## 5. Overnight run checklist (second machine)

Getting the build up on a fresh Mac:

1. **Toolchain**: install CMake (`brew install cmake`) — it was missing on the
   first machine too. If `cmake -B build -G Xcode` complains about the compiler,
   the Xcode license needs accepting (`sudo xcodebuild -license accept`) or
   build with `-G "Unix Makefiles"` against the Command Line Tools, which is how
   every build so far was made.
2. **Copy the repo WITHOUT `build/`** — it holds the multi-hundred-MB JUCE
   clone and is fully regenerated. First configure on the new machine
   re-fetches JUCE and needs network.
3. **Settings do not travel.** Device selection, channel names, Monitor flags,
   the log destination and lane layout live in
   `~/Library/Application Support/PinkNoiseAudioMonitor/` on the old machine.
   Reconfigure on the new one (different interface anyway).
4. Grant the microphone permission on first launch or every input reads silence.

Before walking away for the night:

1. Audio Device Setup: pick the interface, set channel counts (OUT CH / IN CH
   are independent in the status bar), name every channel, and **untick Monitor
   on any input with nothing patched in** — a floating input sits below the
   dropout threshold forever and, with noise playing, picks up crosstalk at
   roughly −50 dBFS (measured ~33 dB isolation on a UMC404HD; see the mute test
   in the session notes). Unmonitored inputs are excluded from
   measurements.csv entirely.
2. Set the log file (Logging menu, Cmd-Shift-L) — or enable "Ask for log name &
   location at each session start" in Settings if per-run prompting is wanted.
3. Start Audio (Cmd-R), confirm every REF OUT bar moves and each patched input
   returns signal; Start Logging (Cmd-L); drop a marker (Cmd-Shift-M) and
   double-click its row to name it — this also proves events.csv is writable.
4. **Stop the machine sleeping.** Display sleep is fine; system sleep kills the
   session. Either System Settings -> Energy -> prevent automatic sleeping, or
   leave `caffeinate -s` running in a terminal.
5. Desktop notifications only appear on a signed build with the notifications
   entitlement (see section 3) — do not rely on them for the first run; the CSVs
   and event log are the record.

What a healthy morning log looks like: elapsed_s strictly increasing with no
backwards jumps, one start/end pair per genuine fault (never a repeating
started/ended stream), dropout_flag zero except during real faults, and
measurements.csv columns only for the channels that were monitored. The first
overnight target from the spec: no crash, no leak, flat memory, and graceful
recovery if the device hiccups.

## 6. Releasing an update (what the in-app updater expects)

The app updates itself from this repo's GitHub **releases**, not from commits:
pushing code changes nothing for users until a release is published. Each
running copy asks the GitHub API for the latest release shortly after launch
and once a day, and offers to install it if its version is newer.

The contract `Source/Update/UpdateChecker.cpp` depends on:

- The tag is `v<major>.<minor>.<patch>` and matches `project(... VERSION ...)`
  in `CMakeLists.txt`. Bump that first; the updater discards a download whose
  bundle version is not the tag's.
- The release has one asset named `*-macOS-universal.zip` that unzips to a
  single `.app`.
- That app is signed with the Developer ID of team `U6N5RLQS69` under the
  hardened runtime, with the generated entitlements file (it must contain
  `com.apple.security.device.audio-input`, or every input reads -inf). Anything
  else, including an ad-hoc build, is rejected by the updater.
- Notarize and staple it too, so a first-time browser download opens cleanly.
- Drafts and pre-releases are ignored (the API's `latest` skips them).

Steps, from a clean checkout of the commit being released:

```sh
cmake -B build-release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build-release -j
APP="build-release/PinkNoiseAudioMonitor_artefacts/Release/Pink Noise Audio Monitor.app"
vtool -show-build "$APP/Contents/MacOS/Pink Noise Audio Monitor" | grep minos    # expect 12.0, twice
codesign --force --options runtime --timestamp \
  --entitlements build-release/PinkNoiseAudioMonitor_artefacts/JuceLibraryCode/PinkNoiseAudioMonitor.entitlements \
  --sign "Developer ID Application" "$APP"
ditto -c -k --keepParent "$APP" upload.zip
xcrun notarytool submit upload.zip --keychain-profile <profile> --wait
xcrun stapler staple "$APP"
ditto -c -k --keepParent "$APP" PinkNoiseAudioMonitor-<version>-macOS-universal.zip
gh release create v<version> PinkNoiseAudioMonitor-<version>-macOS-universal.zip --title "..." --notes "..."
```

A copy that was opened straight from a browser download runs from a read-only
"translocated" path and cannot replace itself; the updater then shows the new
version in Finder instead. Keeping the app in `/Applications` avoids that.

## 7. Artifact detection and the October 2026 fixes

### 7.1 What the artifact detector reports

`Source/Core/ArtifactDetector` compares each monitored return against the
reference that was actually sent. New event types (appended, so older numeric
codes are unchanged):

| Event | Meaning | Start/end or instant |
|---|---|---|
| `Click` (8) | An isolated impulse in the return: click, pop, tick, or a burst under ~60 ms. | instant |
| `Noise` (9) | Non-reference energy spread evenly in time: static, hiss, crackle, buzz. Also raised when clicks arrive faster than 20 a second. | start/end; instant for a burst of corrupted audio |
| `Distortion` (10) | The same rise, concentrated on the signal's own peaks: clipping or saturation somewhere in the path. | start/end |
| `TimingSlip` (11) | The path's delay stepped: samples dropped or repeated. The size is reported in samples. | instant |
| `SignalMismatch` (12) | The return is present but is not the reference: wrong routing, another source, destroyed audio. Critical. | start/end |

`measurements.csv` gains three columns per monitored input: `_latency_ms`
(round-trip latency), `_residual_db` (how much of the return is NOT the
reference, relative to the part that is; a THD+N-like figure) and
`_click_count` (every click in the interval, including those not given their own
event). Latency and residual are **empty** while a return is not aligned; empty
never means zero.

Deliberately not artifacts: a change in level or polarity, a dropout, a muted
output, steady clock drift. The level detectors report the first two.

Limits, stated plainly:

- The comparison band is about 200 Hz to 16 kHz. **Pure mains hum (50/60 Hz) or
  a slow thump is not reliably reported.** Buzz with harmonics is.
- Sensitivity depends on how well the path can be modelled. On the simulated
  converter path in the test rig the residual floor is about -45 to -49 dB;
  clicks down to 20 dB below the signal's RMS and hiss 30 dB below it are
  caught. A path with heavy processing (EQ, crossover, dynamics) will have a
  higher floor and proportionally lower sensitivity; the detector learns the
  floor and reports only rises above it.
- **Dynamics processing in the path is not supported.** A compressor or limiter
  changes gain continuously in a way no fixed model can follow; an independent
  review measured 1,500 to 2,100 false Click events in ten minutes through one.
  Switch artifact detection off in Settings ("Detect clicks, noise, distortion
  and timing slips") for such a path, or bypass the dynamics for the test.
- A path the model cannot follow closely (a steep high-pass, a resonant EQ,
  hum already present) raises no false events, but its residual floor is high
  (about -13 to -16 dB in the test rig against -45 on a clean path), so only
  gross faults will show there. `_residual_db` in the log tells you which
  situation you are in.
- Clocks that are not locked to each other: drift up to about 100 ppm is
  followed without events, at a cost in sensitivity (the floor rose to about
  -25 dB at 50 to 100 ppm). At 150 ppm and beyond it produces false
  TimingSlips. Real interfaces are within a few tens of ppm.
- Round-trip latency up to about 0.65 s can be found.
- While a return is more than 20 dB below its expected level the comparison is
  suspended (that is the dropout detector's territory): artifacts in that state
  are not reported, and the latency/residual cells are empty.
- Cost measured on an M4 Pro, every channel aligned, as a share of the callback
  deadline at a 512-sample buffer: 8 channels at 48 kHz 1%; 16 at 96 kHz 4%;
  32 at 96 kHz 8%; 64 at 192 kHz 34% (worst callback 70%). Fine for ordinary
  rigs; at the very top of that range consider switching it off.

### 7.2 What was verified, and how

- **Offline, by fault injection** (`Tests/ArtifactDetectorTests.cpp`, all checks
  passing): clicks from +20 to -20 dB re signal RMS; slips of -4096, -512,
  -64, -1, +1, +64, +1024 samples (each reported once, size right to a fraction
  of a sample); white noise at -10/-20/-30 dB; crackle; hard and soft clipping;
  buzz; an unrelated signal; corrupted bursts of 20/150/800 ms; and that a 10 dB
  level drop, a 6 dB rise, a 2 s dropout, a mute, a polarity flip and 20 ppm of
  clock drift for 5 minutes report nothing. 80 minutes of clean signal across
  48/96/192 kHz and 64 to 1024-sample buffers: zero events.
- **Independently reviewed.** A second reviewer built the detector separately
  and attacked it with its own cases. That found, and the rig now pins down:
  noise as loud as the signal going unreported while it lasted; false clicks
  through a steep high-pass, a resonant EQ or pre-existing hum; a Noise start
  that never ended after the output level was lowered 20 dB; slips lost when
  many channels slipped at once; a stale model after re-monitoring a channel;
  an inverted path aligned on a sidelobe; and one NaN sample blinding a
  channel. The `review` test group covers each.
- **Not verified:** see the "Hardware status" line at the end of this section.

### 7.3 Reliability fixes from the two code reviews

Level detection (`EventDetector`):

- Dropout is now relative to the expected level (20 dB below what the reference
  and baseline predict, or the absolute floor) with exit hysteresis. The old
  absolute -60 dBFS test sat 0.85 dB above this rig's muted floor: a model of
  it flapped 131 start/end pairs in 10 minutes of one continuous loss, and a
  pulled cable reading -50 dBFS from crosstalk never registered. Now: one pair.
- Level change is judged on smoothed energies with an end hold, so steps of
  8 to 15 dB are each one pair instead of missed, late or repeated. No
  level-change evaluation during a dropout.
- A Start is retried until queued; an End is never rate-limited.

Engine and devices:

- An unexpected device stop holds the output silent. Previously JUCE fell back
  to the system default device and the app kept playing pink noise into it and
  logging against it. The UI shows it in red, tries to get the saved interface
  back, and resumes when it returns. Start Audio asks before playing through a
  device that is not the saved one.
- `running` survives a device restart, so Cmd-R can no longer zero the session
  clock under an open log. A device start can no longer undo Mute.
- The "expected device change" bracket (§3c) now covers only an actual
  user-initiated change plus one second, not the lifetime of the Audio Device
  Setup dialog: leaving that dialog open no longer hides a real device loss.

Logging:

- Timestamps re-anchor to the wall clock when the audio clock and wall clock
  diverge by more than 2 s (sleep, a stalled device), with one row saying so.
- A write error is retried every 3 s instead of ending logging for the session.
- Pause now gates measurements only: events and markers are still written, and
  "Logging paused" / "Logging resumed" rows mark the gap (supersedes §1).
- A fault already active when Start Logging is pressed is written to the new
  `events.csv`.
- `events.csv` is rewritten atomically when a note is amended.
- A system-sleep assertion is held while logging, replacing the manual
  `caffeinate` step in §5.

UI:

- The output-level slider no longer shows -20 dBFS after relaunch while the
  generator plays the saved level.
- Detection thresholds and notification settings persist.
- Stop Audio, Stop Logging and Quit ask first while a session is being logged.
- Start Logging runs a pre-flight and lists anything that would waste the run
  (muted output, a monitored input at digital silence, a fault already active,
  an unwritable or nearly full log folder).
- A watchdog raises a Critical row if a running session stops delivering
  measurements for 3 s.
- Unmonitored inputs are drawn dimmed with "OFF". Errors get an alert and a
  full-width red status instead of a 90-pixel cell.

### 7.4 Hardware status

Run on 2026-10-10 on the M4 Pro Mac mini with a UMC404HD, Out 1 cabled to In 1,
48 kHz / 512, the app signed with the Developer ID under the hardened runtime.

Verified on that hardware:

- The detector aligns within seconds of Start Audio. Round-trip latency read
  22.929 to 22.930 ms for the whole of every run; residual about -52 to -53 dB
  (better than the simulated path's -45).
- Several runs of five to twelve minutes at -20 dBFS: no dropout, level, noise,
  distortion, slip or mismatch events.
- Mute and unmute mid-run: no events; the latency/residual cells are empty
  while muted and resume on the same alignment.
- Output at -40 dBFS: 47 s with no events. Before the dropout fix in §7.3 this
  exact case logged 15 dropout start/end events in 8 seconds; it was found on
  the hardware, not in simulation.
- Quit while logging asks first with "Keep Logging" as the default, and the run
  continues. The sleep assertion is held while logging (`pmset -g assertions`).
- The bundled icon is drawn correctly by the system at 16 to 256 px.

Found on that hardware, and worth knowing about this rig: **a pair of faint
ticks every 128.0 seconds** (two ticks 0.31 s apart, about -66 dBFS peak, 2 to 4
samples long, roughly 24 dB under the signal). Six consecutive pairs arrived
128.001 s apart to the millisecond. Nothing in the app runs on that schedule
and the ticks are not aligned to audio buffers, so they are in the
interface/USB path, not the software. They are far too small to hear. This is
why clicks come in two tiers: one at least 20 dB-under-signal big is an event;
smaller ones are counted in `_click_count` every interval and summarised in one
Info row at most once an hour.

NOT verified on hardware (simulated only, or not exercised at all):

- Every injected fault: no real click, noise, distortion or timing slip has
  been put through the interface, and no cable was pulled.
- Device loss and recovery (unplugging the interface, the fallback hold, the
  automatic return to the saved interface). This code was written from two code
  reviews and compiles; none of it has been run against a real unplug.
- More than one channel, 96 or 192 kHz, and any run of 1.1.0 longer than about
  twelve minutes. The 60-hour run in July was 1.0.0.
- An Intel Mac, or any macOS older than the build machine's.

