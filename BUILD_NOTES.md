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
