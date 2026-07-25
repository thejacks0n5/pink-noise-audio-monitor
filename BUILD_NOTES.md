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
5. **Audio device**: the app opens the *default* input/output device on
   Start Audio (best-effort stereo, falling back to mono — see
   `AudioEngine::start()`). For a genuine loopback/monitoring test you need
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
