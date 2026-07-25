# CLAUDE.md — Pink Noise Audio Monitor

Context for Claude Code (or any coding agent) picking up this project. Read this first.

## What this is

A professional audio testing/monitoring app for **macOS (Apple Silicon)**, written in
**C++20 + JUCE 8.0.4**. It generates deterministic pink noise out an output, captures the
returned signal on an input, aligns/compares them, and logs audio problems (dropouts,
clipping, level changes, …) for long overnight test sessions.

This repo is the **Version One foundation**, generated as a clean, architected scaffold.
It has **not been compiled on a Mac yet** — the first job is to get it building in Xcode on
Apple Silicon and fix whatever the compiler finds. See `BUILD_NOTES.md` for known soft spots
the generator already flagged.

**Target machine:** Mac mini, M4 Pro (Apple Silicon). Xcode + Core Audio + a real audio
interface (or a loopback device like BlackHole) are needed to actually run and test it.

## Scope discipline — IMPORTANT

Build **Version One only.** Do NOT start on advanced features until V1 compiles, runs
overnight, and is dependable. V1 = device selection, deterministic pink noise + level,
peak/RMS meters, basic waveform, start/stop logging, CSV measurement + event logs,
dropout/clip/level-change detection, live event log, basic macOS notification, stable
long-session operation.

Explicitly **out of scope for now** (later phases): FFT/spectrum, transfer function,
frequency-response/EQ/filter detection, compression/limiting detection, delay/correlation
alignment beyond the stub, LUFS, session reports, email/phone notifications, and the VST3
plugin. The architecture leaves room for these, but don't build them yet.

## Architecture

Namespace `pnam` throughout. Modules under `Source/`:

- `Model/DataModel.h(.cpp)` — `EventType`, `Severity`, `ChannelId` enums; the trivially-copyable
  `Measurement` and `Event` POD structs (these are the FIFO payloads AND define the CSV columns);
  `SessionInfo` (message-thread only). **The CSV schema is derived from these structs — keep them in sync.**
- `Core/MeasurementQueue.h` — header-only lock-free SPSC queues (`juce::AbstractFifo`) for
  `Measurement` and `Event`. Audio thread pushes; message thread drains.
- `Core/PinkNoiseGenerator` — seeded xorshift64* + Paul Kellet economy pink filter. Deterministic:
  same seed ⇒ identical output. Level in dBFS (default −20), mute, fade in/out, mono/stereo.
- `Core/ReferenceHistory` — preallocated ring buffer of recent reference samples for comparison.
- `Core/AnalysisProcessor` — audio-thread per-block peak/RMS/gain-diff → emits `Measurement`s; exposes
  atomic getters the UI polls for live meters.
- `Core/EventDetector` — audio-thread threshold checks (dropout / clip / level change) with
  debounce + rate-limiting → emits `Event`s (start/end, not per-block).
- `Core/AudioEngine` — `juce::AudioDeviceManager` + IO callback: generator → output, capture input,
  feed analysis + detection, push to queues. **No allocation or file I/O in the callback.**
  Device lifecycle: `initialiseDevices (savedStateXml)` is called once from `MainComponent`'s ctor so
  a real device is open (and the user's saved choice restored) *before* Start Audio. `start()` only
  attaches the callback / reopens the last device — it must **never** call `initialiseWithDefaultDevices`,
  which would silently discard the user's Audio Device Setup selection. `createDeviceStateXml()`
  feeds persistence.
- `Logging/CsvWriter` + `Logging/SessionLogger` — message-thread CSV writers; per-session folder with
  `measurements.csv`, `events.csv`, `session_info.txt`.
- `Notifications/NotificationEngine` — rate-limited macOS desktop notifications.
- `UI/*` — `DarkLookAndFeel`, `LevelMeterComponent`, `WaveformDisplay`, `EventLogComponent`,
  `TopStatusBar`, `TransportControls`, and `MainComponent` (the integration hub that owns everything).
  `TopStatusBar`'s OUT/IN/CH/RATE/BUFFER cells are click-to-edit: they fire `onCellClicked`, and
  `MainComponent::showStatusBarDeviceMenu` drops a `PopupMenu` that changes the device in place.
  `MainComponent` is a `juce::ChangeListener` on the `AudioDeviceManager` and persists the selection
  to `appProperties` ("audioDeviceState") on every change.
- `Main.cpp` — `juce::JUCEApplication` + `DocumentWindow` hosting `MainComponent`, plus the macOS main
  menu (`MenuBarModel` + `ApplicationCommandManager`). **Preferences lives in the application menu
  (Cmd-,), not on a transport button** — macOS renders it as "Settings…". `MainComponent::openSettings`
  and `openAudioDeviceSetup` are public so the menu can drive them.

## The one rule that matters most (real-time safety)

The audio thread (`AudioEngine::audioDeviceIOCallbackWithContext` and everything it calls) must
**never** allocate, lock, block, write files, post notifications, or touch any `juce::Component`.
It may only do math and push PODs into the lock-free FIFOs. All file I/O, notifications, and UI
updates happen on the **message thread**, exclusively inside `MainComponent::timerCallback()`
(the only place either FIFO is drained). Preserve this invariant in every change. If you need new
data on the UI, add a field to the POD and push it — do not reach across threads.

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

## Suggested first session (Phase 2 plan)

1. **Get it compiling.** `cmake -B build -G Xcode`, build, fix every error/warning-as-error until
   the app launches. Work down the compiler output; don't refactor architecture to dodge an error.
2. **Smoke test on hardware.** Launch, open Audio Device Setup, pick your interface, Start Audio →
   confirm pink noise plays and the reference meter moves. Route output→input (loopback/BlackHole),
   confirm the input meter + waveform respond.
3. **Verify the V1 success criteria** (from the product spec): manually introduce a dropout, a clip,
   and a level change and confirm each is detected, logged to `events.csv` with an accurate timestamp,
   and shown in the live event log — while the UI stays responsive.
4. **Overnight stability.** Run 12+ hours; confirm no crash, no leak, no runaway log growth, and
   graceful recovery if the audio device is unplugged/replugged.
5. Only then move to the next spec phase (delay/correlation alignment, then FFT/frequency analysis).

A good opening prompt for Claude Code: *"Read CLAUDE.md and BUILD_NOTES.md, then build this project
with CMake/Xcode and fix compile and link errors until the app launches. Don't add features — just
get V1 compiling and running. Show me the errors you hit and how you fixed them."*
