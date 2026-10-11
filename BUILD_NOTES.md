# Build Notes — Pink Noise Audio Monitor (pnam)

> **How to read this file.** Sections 1 to 5 were written while Version One
> was being built and are kept as the record of that time; each carries a note
> where later work changed what it says. Sections 6 to 8 cover releasing,
> artifact detection and the delay monitor. Sections 9 to 13 are the full
> project record: what was done and when (§9), every decision and its reason
> (§10), every review finding and its outcome (§11), everything still open or
> unverified (§12), and reference tables (§13). Where an older section and a
> newer one disagree, the newer one is current.

Integration pass: implemented `Source/UI/MainComponent.cpp` against the fixed
`MainComponent.h` contract, and did a consistency pass across every other
`Source/*.cpp` file. This document summarizes what's implemented, what was
fixed, what's stubbed, and a first-build checklist for Apple Silicon.

## 1. What's implemented for V1

> **Since changed:** Pause now gates measurements only, and events are always
> written (§10.6). The queues are also drained once at Start Audio and Start
> Logging. The main window has a DELAY strip under the transport (§8) and no
> MODE cell. The dialogs are modal to the main window, not modeless. Settings
> now also holds "dropout: dB below expected" and the artifact-detection
> switch, and everything in it persists. The audio callback also runs the
> artifact detector (§7, §10.4).

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

> **Since changed:** builds are now signed with a Developer ID and notarized
> (§10.1), but notifications have still never been seen to appear. Clip counts
> are now carried per input channel. Everything else here still stands; the
> consolidated list of open items is §12.

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

> **Since changed:** the "expected device change" bracket now covers a real
> user change plus one second, not the lifetime of the Audio Device Setup
> dialog, and `running` is no longer cleared by a device stop (§10.5). The
> "observed once, not fully diagnosed" case at the end of this section was
> most likely CoreAudio's follow-up restart arriving after the old bracket had
> closed, which those changes address; it has not been re-tested by hand.

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

> **Since changed:** a downloaded release needs none of the build steps
> (README quick start). The app now holds a system-sleep assertion itself
> while it is logging, so `caffeinate` is only needed if audio is left running
> without logging. A floating input no longer "sits below the dropout
> threshold forever" in the same way (dropout is relative now, §10.3), but
> unticking Monitor on unpatched inputs is still right. Start Logging now
> warns about a silent monitored input before the run. Settings are still per
> machine, and delay calibration is per interface, rate and buffer (§8).

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

## 8. Delay monitor and calibration (1.2.0)

The DELAY strip under the transport shows, live, how long the noise takes to
come back on each input. The number is the round-trip latency the artifact
detector already measures to align the return (§7), so there is nothing extra
running; artifact detection has to be on.

To measure a device or a system:

1. Cable the interface's output straight to its input. The strip reads the
   interface's own round trip ("round trip, not calibrated").
2. Press **Calibrate**. That round trip becomes the offset and the strip reads
   0.00 ms. (**Offset...** types one in instead; **Clear** removes it.)
3. Put the device in the path. The strip reads its delay, in ms and samples,
   and follows a change within two or three seconds.

Things to know:

- The calibration belongs to one interface at one sample rate and buffer size,
  and is saved and recalled per configuration. Change the buffer and you are on
  a different (probably uncalibrated) configuration; the strip says so. On the
  UMC404HD at 48 kHz the round trip is 22.93 ms at 512 samples and 12.26 ms at
  256.
- Delays up to about 0.68 s can be found. Beyond that the return is never
  aligned: the strip shows "finding the return..." rather than a wrong number,
  and after 3 s a Signal mismatch is logged.
- The figure is the broadband arrival time between about 200 Hz and 16 kHz.
  Through something with strongly frequency-dependent delay (a crossover, a
  steep filter) it is where most of the energy arrives, not a per-band figure.
- Swapping the device in or out is itself an event to the artifact detector: a
  Timing slip row with the size of the step. That is expected.
- `measurements.csv` has `_latency_ms` (as measured) and `_delay_ms` (minus the
  offset) per input; `session_info.txt` records the offsets the session started
  with, and a change mid-session is an Info row in `events.csv`.

Verified: in the test rig, path delays of 2, 23, 300 and 600 ms at 44.1 to
192 kHz are measured to within 0.015 ms; after calibrating, a device adding
exactly 300 ms reads 300.000 ms 2.5 s after it goes in and 0.000 ms 2.5 s after
it comes out; a 900 ms path is not reported as aligned. On the UMC404HD
(2026-10-10, Out 1 to In 1, -40 dBFS): the strip read 22.93 ms; Calibrate took
it to 0.00; a typed offset of 20 read 2.93; the calibration survived a relaunch
and the round trip repeated to 0.001 ms across three device starts; at a
256-sample buffer it read 12.26 ms, uncalibrated. NOT verified on hardware: a
real device with a known delay in the path, more than one input, and the
calibration switching when the buffer or rate is changed while running (only
the same code path at launch was exercised).

## 9. Project record: what was done, in order

Sections 1 to 5 above were written while V1 was being built and are kept as
they were, with a note at the top of each where later work superseded them.
From here down is the record of everything since: what happened, what was
decided and why, what the reviews found, what is still open, and reference
tables. If a statement here disagrees with an older section, this one is
current.

### 9.1 July 2026: the acceptance run (version 1.0.0)

On the second machine (Mac mini, M4 Pro, Behringer UMC404HD):

- Built from a copy of the repo with CMake (installed with Homebrew) and the
  Unix Makefiles generator against the Command Line Tools. The project's own
  sources compiled without warnings; clang 21 printed about two dozen in
  JUCE's bundled code (harfbuzz and a few `.mm` files), which are upstream.
- Configured for what was actually cabled: one loop, Out 1 to In 1, so one
  output and one input. Microphone permission was already granted and system
  sleep was already disabled on AC power.
- The run started on 2026-07-26 at 00:23 local. In its first minutes the
  sample rate was changed from 48 to 192 kHz (logged as one
  `AudioSettingsChanged` row, clock stayed monotonic), the output level was
  raised to -10.5 dBFS, and one marker was dropped. A small shell script
  sampled the app's memory, CPU and log size every ten minutes into
  `overnight_watch.log`.
- Checked at 60.3 hours: same process, 217,249 measurement rows, no backwards
  step in `elapsed_s`, resident memory between 113 and 143 MB and not rising,
  22 MB of log growing linearly, no dropouts, no clips, and only the two
  day-one rows in `events.csv`.
- It was left running and reached about 12.5 days (1,082,894 rows). It did
  not end through a clean stop: `session_info.txt` has no stop line. Whether
  it was killed, crashed or lost power is not known. The CSVs end on complete
  lines.
- The cable-pull smoke test (step 3 of the suggested session) was skipped that
  night and has not been done since. See §12.

The session logs were saved inside the repo folder, so `PinkNoiseLog_*/` and
`overnight_watch.log` were added to `.gitignore`.

### 9.2 October 2026: publishing, updating, detecting, restyling

All on 2026-10-10, on the same Mac mini.

1. **Published.** The repo went to GitHub. `master` was fast-forwarded to the
   tested `fix/v1-build-errors` tip (24 commits ahead of the original
   scaffold) and both were pushed. `wip/multichannel-foundation` (one commit
   that does not compile) was left local. A clean clone was built to prove
   the download path, which found that the app was being stamped as requiring
   the build machine's own macOS version (§10.1).
2. **Signed and notarized.** The first 1.0.0 download was ad-hoc signed.
   Signing it with a Developer ID under the hardened runtime showed that the
   app then received no audio input at all, silently (§10.1). Fixed, re-signed,
   notarized, and the 1.0.0 download replaced in place.
3. **Made public,** at the owner's request, so the other Mac needs no login.
   The pushed history was scanned first for keys, passwords, addresses and
   paths: none. The author metadata of one commit was then corrected, which
   rewrote that commit and the three after it and moved the `v1.0.0` tag;
   commits up to `f5504a0` are unchanged.
4. **In-app updates** (1.0.1, withdrawn; then 1.0.2). §10.2.
5. **Two code reviews, then fixes, then three independent reviews of the
   fixes and of the new detector.** §11.
6. **Artifact detection:** clicks, noise, distortion, timing slips, signal
   mismatch. §7 and §10.4.
7. **Icon and theme:** designed, audited, corrected, recoloured from pink to
   blue at the owner's request. §10.8.
8. **Hardware checks** on the UMC404HD, which changed two designs (§10.3,
   §10.4) and are listed in §7.4 and §8.
9. **1.1.0** released with 5 to 8. **1.2.0** released with the live delay
   monitor and its calibration (§8, §10.9).

### 9.3 Releases

| Version | Tag commit | What it is |
|---|---|---|
| 1.0.0 | `4e76b12` | V1 as it passed the July run. The download was rebuilt the same day with the audio-input entitlement (one commit later, `abf1c82`), signed and notarized. **Has no updater:** a machine on 1.0.0 must download a newer version by hand once. |
| 1.0.1 | (deleted) | First build with the updater. Its install step was broken (§10.2). Withdrawn within minutes, tag removed. Do not reuse the number. |
| 1.0.2 | `6f28185` | Working updater. |
| 1.1.0 | `406415e` | Artifact detection, the review fixes, blue theme, app icon. |
| 1.2.0 | `7e4adfa` | Live delay monitor with per-configuration calibration. |

Each of 1.0.2, 1.1.0 and 1.2.0 was proved by running the previous published
download in a throwaway home folder and letting it update itself: found,
downloaded, signature checked, installed, relaunched, Gatekeeper-accepted.

## 10. Decisions, and why

### 10.1 Building and distributing

- **Universal binary, macOS 12 or later.** `CMakeLists.txt` forces arm64
  unless told otherwise, so release builds pass
  `-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64`. The 12.0 deployment target was
  set after `project()`, where CMake ignores it; binaries were being stamped
  with the build machine's own OS version and would not have launched on
  anything older. It now sits before `project()`. Check with
  `vtool -show-build` (expect `minos 12.0` twice).
- **Makefiles generator, not Xcode.** Every build so far has been made with
  `-G "Unix Makefiles"`. Release builds live in `build-release/`.
- **Developer ID plus notarization,** so a downloaded copy opens with no
  Terminal step. Ad-hoc signing needed `xattr -dr com.apple.quarantine`.
- **The audio-input entitlement is required.** Notarization requires the
  hardened runtime, and under it macOS denies the input device unless the app
  carries `com.apple.security.device.audio-input`: no prompt, every input
  reads -inf. JUCE was generating an empty entitlements file. It is now set
  through `HARDENED_RUNTIME_OPTIONS`.
- **How to test that honestly.** Launch the bundle with `open`, never by
  running the binary from a shell: a process started from a shell inherits
  the shell's microphone access and hides the problem. To try a first launch
  without touching real settings, launch with the environment variable
  `CFFIXED_USER_HOME` pointing at an empty folder.
- **The repo is public** by the owner's choice. The updater depends on that:
  it reads the GitHub API without credentials, so if the repo is made private
  again, automatic checks will fail silently and Check for Updates will
  report an error.
- **Session logs and build folders stay out of git** (`PinkNoiseLog_*/`,
  `overnight_watch.log`, `build*/`).

### 10.2 The updater

- **It follows releases, not commits.** The request was for the app to update
  "when the repo is updated". Pushing code changes nothing for users; a
  release with a new version number does. This was a deliberate reading: a
  monitoring tool should move between tested, signed builds, not between
  commits.
- **It never installs or quits by itself.** It downloads and verifies in the
  background, then asks. An update must not be able to end an unattended
  run. The prompt says that installing ends any running session.
- **Automatic checks:** five seconds after launch and then once a day, in
  Release builds only (a Debug build does not phone home, but Check for
  Updates still works). An automatic check that fails says nothing, because
  a rack machine may be offline; a check the user asked for always answers.
- **What is accepted:** only a bundle that `codesign` confirms is signed with
  our Developer ID team and bundle identifier, whose version equals the
  release's tag and is newer than the running one. Anything else is deleted.
- **Unpacked with `ditto`,** not JUCE's zip reader, so the bundle's symlinks,
  permissions and extended attributes survive and the signature stays valid.
- **Installed by a detached shell script after the app has quit:** wait for
  the process to go, move the old bundle aside, copy the new one in, restore
  the old one if the copy fails, reopen.
- **1.0.1 was withdrawn** because that script could not run. JUCE's
  `replaceWithText` writes Windows line endings by default, and `sh` cannot
  parse them: the app quit and nothing reopened it. Only a real end-to-end
  update showed it. The script is now written with Unix line endings and
  syntax-checked with `sh -n` before the app is allowed to quit.
- **A copy run from where it cannot replace itself** (straight from a browser
  download, which macOS runs from a read-only translocated path, or a folder
  the user cannot write to) is shown the new version in Finder instead. Keep
  the app in Applications.
- **The updater quits directly** rather than through the "Quit while
  logging?" confirmation added later: the update prompt has already said the
  session ends, and the install script only waits one minute for the process
  to go.
- **Release notes are written as plain text.** The update prompt shows the
  first 500 characters of the release body exactly as written, so Markdown
  asterisks would appear literally.

### 10.3 Level detection (dropout, level change)

- **Dropout is judged against what the return should be,** not against a
  fixed level. With a baseline captured, a monitored input is in dropout when
  it is `dropoutRelativeDb` (default 20 dB) below the level the reference and
  the learned path gain predict. The old test was "below -60 dBFS", and this
  rig showed why that cannot work: its muted floor is -60.85 dBFS (0.85 dB
  under the threshold, so one continuous loss flapped), and a pulled cable
  reads about -50 dBFS from crosstalk (10 dB over it, so it never fired).
- **Relative only, once a baseline exists.** The first version of the fix
  used "the higher of the absolute floor and the relative level". On the real
  interface at a -40 dBFS output level that logged 15 dropout events in 8
  seconds on a healthy return, because the absolute floor won and ordinary
  block-to-block variation of pink noise crossed it. The absolute
  `dropoutThresholdDb` now applies only in the first seconds, before a
  baseline exists, and even then as the lower of the floor and
  (reference minus 20 dB).
- **Exit hysteresis is capped.** A dropout ends when the input is back above
  the entry level by `min (6 dB, half the distance to the predicted level)`,
  held for `minDropoutMs`. A fixed 6 dB could never be reached on a quiet
  return, so the dropout never ended.
- **The prediction follows the reference down at once** (the lower of the
  smoothed reference and the current block), so turning the output down by
  more than 20 dB is not a dropout.
- **The input is judged per block, not smoothed.** A smoothed input would
  take about 1.8 s to fall 20 dB and would miss every short dropout.
- **Level change uses smoothed energies** (one-pole, 400 ms, on input and
  reference) and must be back within tolerance for `levelChangeHoldMs` before
  it ends. Raw per-block figures were too noisy: steps of 8 to 15 dB were
  missed, late, or reported and immediately "ended".
- **No level-change evaluation during a dropout,** so one fault is one pair,
  not two.
- **A device restart keeps the baseline of a channel with an active level
  change.** Otherwise the faulty level became the new normal and the fault
  "ended" while still present.
- **No event is lost to the rate limiter.** A Start is stored and re-offered
  every block until it is queued; an End is never rate-limited and waits for
  its Start.
- **Known consequences, accepted:** see §12.2.

### 10.4 The artifact detector

What it is: for each monitored input, find the round-trip delay, learn the
path's linear response, subtract the prediction from the return, and inspect
what is left. Decisions made on the way, each from a measured failure:

- **Delay by GCC-PHAT cross-correlation, on a background thread.** The audio
  thread only captures into a preallocated buffer and hands it over through
  one atomic per channel; it never signals, locks or waits. A full search
  covers about 0.68 s; after a loss, a short search around the old delay is
  tried first.
- **A 32-tap NLMS model** follows gain, polarity, fractional delay and gentle
  EQ.
- **Both signals are band-limited to about 200 Hz to 16 kHz** before
  comparison. Below that, converters' AC coupling shifts phase where pink
  noise has most of its energy; above it, anti-alias filters reshape the
  band. Leaving either in cost tens of dB of sensitivity. The price is that
  an artifact living only below about 200 Hz (pure mains hum) is not reliably
  reported.
- **One extra term, a leaky integral of the reference,** cancels the residue
  AC coupling still leaves inside the band. It took the floor from about -35
  to about -45 dB in simulation. It is scaled and adapted as one more
  normalised tap, and slowed after the first second, because early versions
  either diverged or produced slow bumps that read as clicks.
- **The adaptive step is regularised** by the long-run reference power. A
  window that happens to be nearly empty otherwise gives a huge step, the
  model lurches, and the residual spikes exactly like a click.
- **A click is judged on the sample-to-sample change in the residual,** not
  its size. What the model cannot quite follow at the bottom of the band is
  large but smooth; a click is the opposite.
- **The click threshold has three floors,** the highest wins: 8 sigma of the
  residual's own sample-to-sample spread; 40 dB below the signal; 6 times
  the residual's level. The third was added after a review showed hundreds of
  false clicks through a steep high-pass or a resonant EQ.
- **A click is held for 100 ms before it is reported.** If the return is lost
  or found to have slipped right after, it was the leading edge of that
  fault and is reported as that, once.
- **A run of outliers longer than 10 ms is not a click.** If the error is
  just the signal scaled (coherence 0.95 or more), the path's gain or
  polarity changed: the model is rescaled on the spot and nothing is
  reported (that is the level detector's event). If the same model fits with
  the return moved by up to 4 samples either way, it is a small timing slip,
  fitted on the spot and reported with its size. Otherwise something longer
  has started.
- **Twenty or more clicks a second is Noise,** not sixty separate clicks.
- **Noise or Distortion** is a residual 10 dB above its learned baseline for
  200 ms, ending after 500 ms back within 4 dB. It is called Distortion when
  the residual is 6 dB stronger on the signal's own peaks.
- **The baseline is the lowest of three half-second means,** so a burst
  during capture cannot become normal. It then follows the residual down
  (never up) as the model keeps refining, and is re-learned when the signal
  level moves by 6 dB, because the residual is measured relative to the
  signal and the path's own noise floor sits closer at a lower level.
  Without that, turning the output down raised a Noise start that never
  ended.
- **Losing and re-finding the return** is classified when it is found again:
  at a different delay it is a Timing slip (samples dropped or repeated); at
  the same delay it was a burst of something that was not the signal.
- **Present but unmatched for 3 s is a Signal mismatch** (Critical). The timer
  survives alignments that fail verification, because a return that still
  correlates but cannot be modelled (noise as loud as the signal) aligns and
  fails over and over; restarting the timer each time meant such a fault was
  never reported while it lasted.
- **Deliberately not artifacts:** a gain or polarity change, a dropout, a
  muted output, steady clock drift.
- **Two tiers of click,** decided by the hardware. This interface produces a
  pair of faint ticks every 128.0 seconds (§12.4). They are real, so they are
  counted; they are far below audibility, so they must not fill the log. A
  click at least 20 dB-under-signal big is a Warning event. Smaller ones are
  counted in `_click_count` every interval and summarised in one Info row:
  the first at once, then at most one an hour.
- **A channel that stops being processed forgets its model** (unmonitored,
  detection switched off, reference mapping changed, input no longer on the
  device), and anything it had open is closed with an End.
- **Timing slips are exempt from the rate limiter** and retried until queued:
  when every channel slips at once, each one has to be on record.
- **Non-finite samples are zeroed before any filter.** One NaN from a virtual
  device otherwise poisons the filter state for good.
- **The hot loop is written as four independent sums** so the compiler can
  vectorise it.
- **It can be switched off** in Settings, for a path where the return is
  deliberately not a copy.

### 10.5 Devices

- **`isRunning()` means the session,** from Start Audio to Stop Audio. A
  device stopping no longer clears it; `isDeviceActive()` is the device.
  Before, any CoreAudio restart left the app believing it was stopped while
  noise still played, and Cmd-R then zeroed the session clock under an open
  log.
- **An unexpected device stop holds the output silent.** JUCE falls back to
  the system default device; the app used to keep playing pink noise into it
  and logging against it. The hold releases by itself only when a device of
  the same name starts again; otherwise only when the UI clears it or the
  user does Stop then Start.
- **The saved interface is the user's choice and is never overwritten by a
  fallback.** The UI keeps the chosen device state separately from what
  happens to be open, tries to get the chosen one back (backing off 2, 5, 15,
  60 s when it is listed but will not open), and asks before Start Audio
  plays through anything else. Accepting a fallback is remembered by device
  name, for that device only.
- **An invariant enforced every tick:** a session running on a device that is
  neither chosen nor accepted, with no hold in place, is put on hold and
  logged as Critical. This closed a hole where a second drop inside a
  one-second "expected change" window played noise through the Mac's
  speakers with no record.
- **"Expected device change" covers a real user change plus one second,** not
  the whole time the Audio Device Setup dialog is open. A user's own change
  is recognised by the device manager's explicit state having changed, which
  JUCE rewrites only for a chosen change.
- **Only the user's own change may end a session.** An automatic recovery
  that fails logs a row and keeps trying.
- **Switching back is automatic when idle,** and mid-run when the fallback was
  not accepted. A run the user knowingly started on a fallback is left alone
  unless the engine is holding the output.
- **Trouble shows at once in red; the alert box waits two seconds,** so a
  sub-second restart gets a red flash and its log rows but no dialog.
- **A device CoreAudio stops but leaves listed** is closed and restarted after
  3 s, with the same backoff.
- **A device start cannot undo Mute.**
- **The stored reference mapping is never clamped in place.** A spell on a
  two-output fallback used to rewrite a four-output rig's mapping for good.

### 10.6 Logging

- **Pause gates measurements only.** Events and markers are still written,
  and "Logging paused" / "Logging resumed" rows mark the gap.
- **A fault already active when Start Logging is pressed** is written to the
  new log, marked as already active. Only unresolved starts are replayed, not
  the whole history.
- **Timestamps:** `elapsed_s` is the audio clock and stays monotonic.
  `timestamp_utc` follows the wall clock: ordinary drift between the two is
  slewed away a little per row; a real gap (sleep, a stalled device) is
  stepped forward with one Info row saying so; timestamps never go backwards.
  The logger is told where the audio clock is each tick so it can tell a
  fresh row from a backlog.
- **A write error is retried every 3 s,** not latched for the session, and
  recovery is declared only after a real row has been written and flushed.
- **`events.csv` is rewritten atomically** when a note or marker name is
  amended.
- **Lost data is said out loud.** If the audio thread's queues ever overflow,
  one Warning row says how many measurement rows and events were lost.
- **Rows the logger writes on its own account** (clock re-anchored, writing
  recovered) also appear in the on-screen log.
- **A system-sleep assertion is held while logging,** replacing the manual
  `caffeinate` step. It is not held when audio runs without logging.
- **The `timestamp_utc` column name was left alone** although the values are
  local time with an offset. They are unambiguous, and renaming a column
  breaks anything reading old logs.
- **The seed is written and parsed as unsigned,** so a logged seed can be
  typed back in.

### 10.7 Interface behaviour

- **Stop Audio, Stop Logging and Quit ask first while a session is being
  logged.** The safe button ("Keep Logging") is first, has the focus, and
  takes both Return and Escape; the destructive one has no key.
- **Start Logging runs a pre-flight** and lists only what applies: output
  muted; a monitored input at digital silence (which is also what a denied
  microphone permission looks like); no input monitored; a fault already
  active; a device loss in progress; the log folder unwritable or under 1 GB
  free. With nothing to report there is no dialog.
- **Errors get an alert and a wide red status line.** They used to go to a
  status cell about 90 pixels wide. The static MODE cell was removed to make
  the room.
- **A watchdog** raises a Critical row if a running session delivers no
  measurement for 3 s, and an Info row with the gap when they resume.
- **Settings gained two controls:** "Dropout: dB below expected" (the relative
  threshold that now matters; the old dBFS slider is relabelled as the floor
  used at start) and the artifact-detection switch.
- **Thresholds and notification settings persist.** The level slider no
  longer shows -20 dBFS after relaunch while the generator plays the saved
  level.
- **Unmonitored inputs are drawn dimmed with "OFF".**
- **Menu items show state** ("Start Audio" / "Stop Audio") and are disabled
  when they cannot act.
- **Notes commit when the editor loses focus;** Escape discards. A note that
  could not reach the CSV says so in the row.

### 10.8 Icon and theme

- A separate designer pass produced three icon concepts: **A "Dropout"** (a
  noise band that collapses to a line and recovers, with an amber bracket
  marking the gap), **B "Flat octaves"** (bars with one missing) and **C
  "Slope"** (a diagonal on a grid). A was recommended as the only one that
  says "fault" rather than just "audio"; B reads as a generic bar chart and C
  as "disabled" at small sizes. An independent audit agreed with the ranking.
- **The owner chose A and the theme, but not pink.** Three blues were offered
  ("razor blue" not being a defined colour): electric `#1FB0FF`, steel
  `#8DB0CC`, and a truer blue `#3391FF`. **The owner chose the third.**
- **The icon ships as a compiled asset catalog,** not as a plain `.icns` and
  not through JUCE's `ICON_BIG`. The audit found that current macOS draws a
  plain `.icns` shrunk onto a dark plate at small sizes, and that JUCE given
  one PNG discards the hand-fitted 16 and 32 px artwork. The body was also
  redrawn to Apple's template shape for the same reason. The real built app
  was then asked for its icon at 16 to 256 px and drew correctly.
- **With a blue accent, the reference trace and Info rows were moved out of
  blue** (to near-white and cool grey) so that "selected", "a signal" and "an
  informational row" cannot be confused. Green, amber and red still mean only
  level and severity.
- The audit's two palette suggestions were taken: a stronger RMS overlay on
  the meters and a darker idle lamp.
- Known weaknesses that need code rather than a colour are in §12.5.
- Sources and the full reasoning: `docs/design/`.

### 10.9 Delay monitor

- **It shows the latency the detector already measures;** nothing extra runs.
  The number is published only once an alignment has been verified, so a
  candidate is never shown as a measurement.
- **Calibration is an offset per input,** set by pressing Calibrate with the
  output cabled straight to the input, or typed in.
- **Offsets are saved per interface, sample rate and buffer size,** because
  the interface's own latency depends on all three (this rig at 48 kHz:
  22.93 ms at 512 samples, 12.26 ms at 256). A different configuration loads
  its own offsets, or none, and the strip says "not calibrated".
- **Both figures are logged:** `_latency_ms` as measured and `_delay_ms` minus
  the offset, so a calibration mistake can be undone afterwards.
- **What happens if the latency changes after calibration** (asked by the
  owner):
  - A sudden step is logged as a Timing slip with its size in samples and ms,
    down to one sample. Verified in simulation.
  - A slow drift produces no event. The detector follows it, because two
    unlocked clocks drifting is normal. It shows only as `_delay_ms` moving.
  - A change across a device restart, a rate or buffer change, or Stop then
    Start produces no Timing slip, because alignment starts from scratch
    there. The restart is logged and the new figure is in the columns. (From
    the code; not exercised.)
  - A Timing slip is a Warning, so it does not raise a desktop notification.
  - Nothing compares the live delay with the calibrated value. A "delay moved
    more than X ms from calibration" event was offered and has not been
    built; see §12.1.

### 10.10 How the work was done

- **Scope.** V1 was "build Version One only". Everything since (updater,
  artifact detection, delay monitor, theme) was added because the owner
  asked for it. The rule now: add what is asked for, and nothing else.
- **Verify, then claim.** Each feature was built, run, and looked at on
  screen. Statements in these notes say whether a thing was verified by
  simulation, on the real interface, or only by reading the code.
- **Independent review.** The first two reviews found defects; their authors
  then fixed them; three further reviewers, who had not seen the fixes being
  made, attacked the result and found more (§11). The fixes made in response
  to those second reviews, and everything in 1.2.0, have not had a third
  look.
- **Hardware first, quietly.** Before playing through the interface the loop
  was confirmed at -40 dBFS. Two designs were changed by what the hardware
  did that simulation had not shown.
- **Releases are deliberate.** Because installed copies update themselves,
  publishing a release ships it. Each one was tested, signed, notarized, and
  proved by a real self-update before being called done.

## 11. The reviews: every finding and what became of it

"Fixed" means changed in the code and, where a harness existed, re-run.
Severities are the reviewers'.

### 11.1 First review: audio core and logging

It also analysed the real 12.5-day log and modelled the detectors.

| # | Finding | Outcome |
|---|---|---|
| 1 | Critical. Dropout was an absolute -60 dBFS per-block test with no hysteresis; on this rig a continuous loss flapped (131 start/end pairs in ten minutes in a model) and a pulled cable never registered. | Fixed (§10.3). |
| 2 | High. Level change compared unaligned per-block RMS; real 8 to 15 dB changes were missed, late, or logged as a blip and absorbed. | Fixed. |
| 3 | High. Unplugging the interface made JUCE open the system default and the engine kept playing noise into it. | Fixed (§10.5). |
| 4 | High. `running` was cleared by any device stop and never restored. | Fixed. |
| 5 | High. `timestamp_utc` was session start plus the audio clock, never re-anchored: a 4-hour sleep stamped every later row 4 hours early. | Fixed (§10.6). |
| 6 | Medium-high. Rate-limited or queue-full events were dropped while the state machines advanced. | Fixed. |
| 7 | Medium. Every dropout over 500 ms also logged a level-change pair. | Fixed. |
| 8 | Medium. Mute was undone by any device start. | Fixed. |
| 9 | Medium. Thresholds and notification settings were not persisted or recorded. | Persisted. **Not** written to `session_info.txt` (§12.1). |
| 10 | Medium. One write failure disabled that CSV for the rest of the session. | Fixed. |
| 11 | Medium-low. Pause discarded events and left no record. | Fixed. |
| 12 | Medium-low. Amending a note rewrote `events.csv` non-atomically, from an in-memory copy that grows without bound. | Rewrite is atomic. The in-memory copy still grows by one entry per event (§12.2). |
| 13 | Low-medium. Level slider showed -20 dB after launch while the generator ran at the saved level. | Fixed. |
| 14 | Low. Two real-time hazards: an unbounded seqlock retry and a lazily allocated `thread_local` on the audio thread. | Fixed. |
| 15 | Low. `timestamp_utc` is local time with an offset, not UTC; the seed was written signed. | Seed fixed. Column name left as is by decision (§10.6). |

### 11.2 First review: interface and usability

| # | Finding | Outcome |
|---|---|---|
| 1 | High. After an auto-recovered device stop the UI said Running, Start Logging silently did nothing, Stop did not silence the noise, and Cmd-R zeroed the clock. | Fixed. |
| 2 | High. A missing interface, at launch or mid-run, silently moved the app to the system default and never came back. | Fixed. |
| 3 | High. Output-level slider wrong after relaunch. | Fixed. |
| 4 | High. Monitor flags were invisible on the main window, keyed by active-channel position, and carried across devices. | Unmonitored inputs are now drawn dimmed with "OFF". **Storage is unchanged:** still keyed by position, not per device (§12.2). |
| 5 | Medium-high. Thresholds not persisted. | Fixed. |
| 6 | Medium. A fault active at Start Logging never reached `events.csv`. | Fixed. |
| 7 | Medium. Pause left no trace and dropped events and markers. | Fixed. |
| 8 | Medium. One write failure ended logging with weak signalling. | Fixed. |
| 9 | Medium. Every error went to a 90-pixel status cell; no alerts anywhere. | Fixed. |
| 10 | Medium. An open Audio Device Setup dialog suppressed device-loss detection for as long as it stayed open. | Fixed. |
| 11 | Medium. Channel-naming rows were built once per dialog open. | Fixed: they rebuild when the device or channel selection changes. |
| 12 | Medium-low. "Audio settings changed" rows were hidden by default; the type filter was off by one. | Fixed; the filter is built from the enum, so new event types appear by themselves. |
| 13 | Medium-low. Notes could silently miss `events.csv`; clicking away discarded the text. | Commit on focus loss; a note that could not be saved says so. Notes made after Stop Logging still cannot reach the CSV (§12.2). |
| 14 | Low-medium. Cmd-L, Cmd-R and Cmd-Q unguarded while logging; menu showed no state. | Fixed. |
| 15 | Low. A blank channel name shifted later names on reload; the lane-count selector did not follow a restored layout. | Fixed. A blank name can still be typed (it falls back to "In n"). |

Its five suggested changes: Start Logging pre-flight (done); stall watchdog
(done); sleep assertion while logging (done); a persistent run summary in the
status bar with log destination, free space, monitored count and critical
count (**not done**); a one-pass setup dialog with a live level beside each
input and an "untick silent inputs" button (rows rebuild; the rest **not
done**).

### 11.3 Independent review of the core and logging fixes

| # | Finding | Outcome |
|---|---|---|
| 1 | Medium. A dropout on a quiet return never ended (fixed 6 dB exit hysteresis unreachable). | Fixed. |
| 2 | Medium. Lowering the output by more than about 20 dB logged a false Critical dropout. | Fixed. |
| 3 | Medium. A failed `measurements.csv` was declared recovered without proving it could be written, about 450 false recovery rows an hour during a persistent failure. | Fixed. |
| 4 | Medium (older than the fixes). A device restart falsely ended an active level change. | Fixed. |
| 5 | Medium-low. Rows right after an audio-clock stall carried the pre-stall time. | Fixed. |
| 6 | Low-medium. `timestamp_utc` stepped backwards 2 s when the interface clock ran fast; 9 ppm reaches that in 60 hours. | Fixed (slewed). |
| 7 | Low. The device-resume row was an End with no Start. | Fixed: it is an instant row. |
| 8 | Low. One non-finite input block disabled level-change detection on that channel for good. | Fixed. |
| 9 | Low. The outage length excluded time asleep. | Fixed. |
| 10 | Low (older). The audio callback was still attached while engine members were destroyed. | Fixed: the destructor detaches first. |

It also raised that a device CoreAudio stops but leaves listed would not be
recovered; the UI now closes and restarts it after 3 s, with the same backoff.

### 11.4 Independent review of the interface fixes

| # | Finding | Outcome |
|---|---|---|
| 1 | High. A second drop within a second of an automatic re-open played noise through the system default with no record. | Fixed (§10.5). |
| 2 | High. A spell on a fallback device permanently rewrote the reference mapping. | Fixed. |
| 3 | Medium. One failed automatic re-open ended the session and the log. | Fixed. |
| 4 | Low. The stall watchdog could fire across a deliberate device change. | Fixed. |
| 5 | Low. "Fallback accepted" was a bare flag: it blocked recovery, and carried over to a different device. | Fixed: accepted by device name. |
| 6 | Low. The confirm's default button was the destructive one. | Fixed. |
| 7 | Low. No backoff when the saved interface was listed but would not open. | Fixed. |

It found no crash or lifetime bug, and listed behaviour changes nobody had
asked for, all kept: the MODE cell is gone; Cmd-L is disabled while audio is
stopped; the app switches back to the saved interface by itself while idle;
Start Audio asks before playing through a fallback; type-filter entries are
named from the enum; Reset also re-learns the artifact baseline; the queues
are also drained once at Start Audio and Start Logging.

### 11.5 Independent review of the artifact detector

It built the detector separately and added its own fault cases. It found no
real-time violation, data race or memory-safety defect.

| # | Finding | Outcome |
|---|---|---|
| 1 | High. A sustained heavy fault (noise as loud as the signal, for ten minutes) produced no event while it lasted. | Fixed. |
| 2 | High. False clicks on healthy paths with ordinary filtering or hum (up to 518 in ten minutes through a steep high-pass). | Fixed. Those paths now report nothing, at a high residual floor (§7.1). |
| 3 | High. A compressor or limiter in the path floods the log (1,500 to 2,100 clicks in ten minutes). | **Not fixed.** Documented as unsupported; switch detection off for such a path. |
| 4 | High. A level change raised a Noise or Distortion start that never ended. | Fixed. |
| 5 | Medium. The shared rate limiter lost Timing slips when many channels slipped at once and left stale state behind. | Fixed. |
| 6 | Medium. Un-monitoring, disabling or remapping left the model live and stale (a false click on every re-monitor). | Fixed. |
| 7 | Medium. Measurement rows claimed "aligned, residual -120 dB" when nothing had been compared. | Fixed: the cells are empty. |
| 8 | Medium. One NaN sample disabled a channel until the next device start. | Fixed. |
| 9 | Low. Clock drift of 150 ppm or more produces false Timing slips; 50 to 100 ppm costs sensitivity. | **Not fixed.** Documented as a limit. |
| 10 | Low. The delay search took only the positive correlation peak, so an inverted path aligned on a sidelobe. | Fixed. |
| 11 | Low. Three rare transitions could strand or falsify an End. | Addressed (Ends are retried; a tracked fault keeps its baseline; removed inputs are closed). By reasoning, not each by test. |
| 12 | Low. The engine's destructor freed the detector with the callback attached. | Fixed. |

It also measured the cost at 64 channels and 192 kHz at 56% of the callback
deadline; the vectorised loop brought the median to 34% (worst callback 70%).
And it noted that `NotificationEngine` still has seven per-type cooldown
slots, so the newer event types share the "Other" slot; harmless while Signal
mismatch is the only Critical artifact, and unchanged.

### 11.6 Design audit

Verdict: ship with changes; icon A is the right pick; the theme values pass.

- Required and done: redraw the body to Apple's template shape; package as an
  asset catalog; correct the ruler so both markers sit on ticks; check the
  result at every size.
- Suggested and done: stronger meter RMS overlay; darker idle lamp.
- Suggested and not done: see §12.5.
- It verified the designer's contrast table (all 28 rows) and colour-vision
  figures against a second model.

## 12. Open items, known limits, and what has not been verified

### 12.1 Offered or identified, not built

- **A "delay moved from calibration" event** with a start and an end, which
  would also cover slow drift and changes across a restart (§10.9). Offered;
  the owner has not answered.
- **Detection thresholds in `session_info.txt`.** They persist, but a log
  does not record which thresholds produced it.
- **A run summary in the status bar:** log destination and free space, "n of
  m inputs monitored", count of Critical events.
- **A live level and an "untick silent inputs" button** in the channel setup.
- **Tests for the level detectors and the logger.** The harnesses the
  reviewers used were throwaway programs outside the repo and are gone; the
  figures quoted in §10.3 and §11 cannot be re-run from here. Only the
  artifact detector has an in-repo test rig.
- **Desktop notifications** have still never been seen to appear (§3).
- **The settings dialog** still exposes only part of the thresholds (§3), and
  none of the artifact detector's except on/off.

### 12.2 Known behaviour, accepted for now

- A loop whose normal return is more than 20 dB below the reference **and**
  under -60 dBFS from the moment audio starts is called a dropout at 3 s and
  never ends. By level alone it cannot be told from a dead input. Raising the
  pre-baseline margin would fix it and lose the dead-input case on this rig.
- Where the noise floor is within 20 dB of the normal return, a total loss is
  logged as a level change (Warning), not a dropout (Critical).
- A level sitting about 15 to 20 dB down can be logged as one level-change
  pair and later also one dropout pair (seen at -17 dB at 192 kHz/512 and
  96 kHz/256 in the model). Each is logged once.
- A real device loss within one second of the user's own device change is not
  reported by the engine. The interface catches it when that second is up (a
  row, the red status, and a hold with a Critical row if a different device
  opened), so it is recorded up to a second late.
- If a fallback device the user accepted drops and later returns while the
  saved interface is still missing, nothing reopens it; the output stays
  held until the saved interface returns or the user does Stop then Start.
- The JUCE selector's "Reset Device" button is logged as a lost/restarted
  pair.
- Channel names, Monitor flags and delay offsets are keyed by the channel's
  position among the active channels. Unticking a lower hardware channel in
  the device selector shifts them all.
- One non-finite input block still produces one measurement row with `nan`
  cells (the analysis stage was not changed).
- After a disk-full episode the "rows written" figure in `session_info.txt`
  can under-count; the "rows lost" figure is an upper bound.
- The in-memory list of written event rows grows by one entry per event for
  the life of a session.
- Notes added after Stop Logging exist only in the on-screen log and say so.
  A note cannot be cleared back to empty.
- Stopping while paused leaves a "paused" row with no "resumed" row.
- With no saved interface at all, the device Start Audio is pressed on is
  taken as the accepted one without asking.
- Alerts block clicks on the main window until dismissed. Audio, logging and
  the timer keep running behind them.
- A copy on 1.0.0 cannot update itself (§9.3).

### 12.3 Limits of the artifact detector and delay monitor

Listed in §7.1 and §8: no pure mains hum; unusable through a compressor or
limiter; high residual floor through steep filters; false slips beyond about
150 ppm of clock drift; delays up to about 0.68 s; nothing reported while a
return is more than 20 dB below expectation; cost at very high channel counts
and rates.

### 12.4 This rig

Measured on the Mac mini's UMC404HD, Out 1 cabled to In 1, on 2026-10-10:

- Loop gain about -5.7 dB. Muted input floor about -60.85 dBFS (from the July
  log). An unpatched input reads about -50 dBFS from crosstalk with noise
  playing.
- Round trip at 48 kHz: 22.93 ms at a 512-sample buffer, 12.26 ms at 256. It
  repeated to 0.001 ms across device starts.
- Artifact residual about -53 dB at a -20 dBFS output; about -36 dB at
  -40 dBFS, where the converters' noise floor is closer.
- **A pair of faint ticks every 128.0 seconds:** two ticks 0.31 s apart, about
  -66 dBFS peak, two to four samples long, roughly 24 dB under the signal.
  Successive pairs arrived 128.001 s apart. Nothing in the app runs on that
  schedule and they are not aligned to audio buffers, so they belong to the
  interface or its USB path. Inaudible. They are why clicks have two tiers.

### 12.5 Theme and icon: needs code, not colour

- Text selection and the caret are still JUCE's default cyan.
- The half-transparent "(no channel selected)" hint is about 2.85:1; a latched
  button's label is about 3.8:1 while pressed; a disabled-and-latched label
  (Mute left on after Stop Audio) is about 1.25:1, effectively unreadable.
- Per-channel trace colours come from rotating one base hue. Outputs are now
  near-white and cannot be told apart by colour; inputs 1 to 4 are similar
  greens; past eight channels the rotation runs into the accent blue, and
  further on into red and amber.
- Info rows have no hue of their own.
- The critical lamp has no cue other than colour; under simulated red-green
  colour-vision deficiency the OK/warning/critical lightness gaps are small.
- The focus ring on a latched button is faint.
- Icon: no 64-pixel master without the ruler (it speckles slightly there); the
  composition sits about 20 px low; no lookalike or trademark search was
  possible; never looked at in the Dock, a Finder list or Get Info, on another
  Mac, on an older macOS, or in the light, tinted or clear icon styles.
- `docs/design/theme_preview.png` predates the DELAY strip.

### 12.6 Not verified on real hardware

- Any injected fault: no real click, noise, distortion or slip has been put
  through the interface, and no cable has been pulled, on any version.
- Device loss and recovery: unplugging, the fallback hold, the return to the
  saved interface. Written from two rounds of review; never met a real
  unplug. The second round of fixes to it was not independently reviewed.
- A real device of known delay in the path of the delay monitor; calibration
  switching when the buffer or rate changes while running.
- More than one channel; 96 or 192 kHz with the artifact detector.
- Any run of 1.1.0 or 1.2.0 longer than about twelve minutes. The long run
  was 1.0.0.
- An Intel Mac, or a macOS older than the build machine's. Only the binary's
  architecture and minimum-OS stamps were checked.
- Sleep and wake during a session; a full disk; a log volume that disappears.
  Logger behaviour for the last two was exercised in a harness, not in the
  app.

## 13. Reference

### 13.1 Events

| Type (code) | Severity | Shape |
|---|---|---|
| `CompleteDropout` (0) | Critical | start / end |
| `Clipping` (1) | Warning; Critical if a run exceeds 1 s | start / end |
| `LevelChange` (2) | Warning | start / end |
| `ManualMarker` (3) | Info | instant |
| `AudioDeviceLost` (4) | Critical when lost unexpectedly; Info for "restarted after N s" | instant |
| `LoggingError` (5) | Critical on failure; Warning or Info on recovery | instant |
| `Other` (6) | Info, Warning or Critical | instant: pause/resume, calibration changes, clock re-anchor, data lost, watchdog |
| `AudioSettingsChanged` (7) | Info | instant |
| `Click` (8) | Warning; Info for the periodic summary of small ones | instant |
| `Noise` (9) | Warning | start / end; instant for a burst of corrupted audio |
| `Distortion` (10) | Warning | start / end |
| `TimingSlip` (11) | Warning | instant |
| `SignalMismatch` (12) | Critical | start / end |

Only Critical events can raise a desktop notification. Numeric codes are
written to the CSV; append new types, never renumber.

### 13.2 Log files

`events.csv`: `timestamp_utc, elapsed_s, event_type, severity, channel,
measured_value, expected_value, difference, threshold_used, code,
description, note`.

`measurements.csv`: `timestamp_utc, elapsed_s, sample_rate, buffer_size`;
then per output `<name>_peak_db, _rms_db`; then per **monitored** input
`<name>_peak_db, _rms_db, _gain_diff_db, _clip_count, _latency_ms, _delay_ms,
_residual_db, _click_count`; then `correlation` (always 0, not implemented),
`dropout_flag, clip_count, status_code`. Latency, delay and residual are empty
while an input is not aligned. The column set is derived per session from the
channel names and Monitor flags.

`session_info.txt`: session id, start time, rate, buffer, channel counts,
device names, seed, output level, log folder, each channel's name and
reference, each input's delay offset, and at stop the stop time and row
counts.

### 13.3 Defaults

Level detection: dropout 20 dB below expected (`dropoutRelativeDb`); absolute
floor before a baseline -60 dBFS; dropout hold 50 ms; clip at 0.999 of full
scale, run ends after 250 ms clean, Critical after 1 s; level-change tolerance
6 dB, hold 500 ms; baseline capture 2 s; settling after start 3 s; 20 Start
events a second.

Artifact detection: click at 8 sigma, at least 40 dB under the signal; a
click 20 dB under the signal or larger is its own event; noise at 10 dB over
baseline (never below -50 dB) for 200 ms, ends after 500 ms; distortion when
the residual is 6 dB stronger on peaks; mismatch after 3 s; 32 taps; band
200 Hz to 16 kHz (or 0.30 of the sample rate if lower).

Other: output level -20 dBFS; measurement interval 1 s; log flush every 5 s
(events on each row); write retry every 3 s; stall watchdog 3 s; update check
5 s after launch, then daily.

### 13.4 Settings file keys

`~/Library/Application Support/PinkNoiseAudioMonitor/PinkNoiseAudioMonitor.settings`:
`audioDeviceState`, `outputChannelNames`, `inputChannelNames`,
`inputMonitored`, `referenceOutputForInput`, `outputLevelDb`, `pinkNoiseSeed`,
`logBaseName`, `logBaseFolder`, `promptLogEachSession`, `dropoutRelativeDb`,
`dropoutThresholdDb`, `levelChangeToleranceDb`, `minDropoutMs`,
`notificationsEnabled`, `notificationCooldownSeconds`,
`artifactDetectionEnabled`, `waveformLaneCount`, `waveformLaneSources`,
`windowState`, and one `delayOffsetsMs|<output device>|<input
device>|<rate>|<buffer>` per calibrated configuration. Downloaded updates wait
in `~/Library/Caches/PinkNoiseAudioMonitor/Updates/`.

### 13.5 Menus and shortcuts

Application menu: About; Check for Updates...; Settings (Cmd-,). Audio: Start
/ Stop Audio (Cmd-R); Audio Device Setup... (Cmd-Shift-A); Channel Names...
(Cmd-Shift-K). Logging: Start / Stop Logging (Cmd-L); Add Marker
(Cmd-Shift-M); Set Log File... (Cmd-Shift-L). Window: Minimize (Cmd-M); Zoom.

### 13.6 Build, test, release in brief

```sh
cmake -B build-release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build-release -j
cmake --build build-release --target pnam_tests -j && ./build-release/pnam_tests_artefacts/Release/pnam_tests
```

Then §6 for signing, notarizing and publishing. Bump `project(... VERSION
...)` first; write the release notes as plain text; afterwards run the
previous release and let it update itself.
