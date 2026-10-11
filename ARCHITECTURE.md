# Pink Noise Audio Monitor — Core Architecture

> **This document is the original V1 design** and is kept as written. The
> thread model and the "audio thread only pushes PODs" rule still hold. What
> has changed since is summarised here; `CLAUDE.md` has the current module
> list and `BUILD_NOTES.md` §10 the reasons.
>
> **Current state (October 2026, version 1.2.0):**
>
> - **Channels.** Not mono/stereo any more: up to `pnam::maxChannels` (64)
>   inputs and outputs, in fixed arrays inside `Measurement` so it stays a
>   POD. The generator makes an independent stream per output. Each input is
>   compared against a chosen reference output and has a Monitor flag. The
>   `inPeakDbL/R` fields in §6 below no longer exist; the CSV column set is
>   derived per session (`BUILD_NOTES.md` §13.2).
> - **Per-callback sequence** is now: generator to outputs (or silence while
>   the output is held) → **`ArtifactDetector::processBlock`** (the return
>   against the exact block just sent) → `AnalysisProcessor::processBlock`
>   (which also collects the detector's per-interval figures into the
>   `Measurement`) → `EventDetector::processBlockStats`.
> - **A fourth module on the audio thread: `Core/ArtifactDetector`.** It
>   aligns each return to its reference, models the path, and reports clicks,
>   noise, distortion, timing slips and signal mismatch, plus latency and
>   residual figures. Its delay search (an FFT cross-correlation) runs on a
>   separate low-priority thread owned by `AudioEngine`; the two exchange a
>   capture buffer through one atomic per channel.
> - **`AudioEngine` has a header** and more state than §5 describes:
>   `isRunning()` is the session, `isDeviceActive()` the device; an
>   unexpected device stop holds the output silent; the callback is attached
>   from launch with the output silenced so input meters are live before
>   Start Audio; the destructor detaches the callback first.
> - **Suppression.** §4 says open conditions are closed while a suppression
>   bit is set. They are not: an active condition is carried across mute,
>   fades, settling and device restarts, so one fault stays one start/end
>   pair.
> - **Detection rules** are no longer simple thresholds: dropout is relative
>   to the predicted level, level change uses smoothed energies
>   (`BUILD_NOTES.md` §10.3).
> - **Clocking.** `elapsedSeconds` is still the audio clock and is carried
>   forward across device restarts. Wall-clock stamps are no longer just
>   `start + elapsed`: the logger slews for drift and steps forward across a
>   sleep or stall, and never goes backwards.
> - **Queues** are drained on the message thread in the 30 Hz timer, and also
>   once at Start Audio and Start Logging. A full queue no longer loses a
>   Start event: the detectors retry until it is queued.
> - **Also on the message thread now:** `Update/UpdateChecker` (with its own
>   worker thread for network and signature checks) and the device-recovery
>   logic in `MainComponent`.
> - **§7's "out of scope" list:** delay alignment is now done (by the
>   artifact detector). `Measurement::correlation` is still always 0.
> - **Platform:** universal binary, macOS 12 or later.

macOS (Apple Silicon) standalone app, C++ / JUCE 8.x (CMake + FetchContent).
Everything lives in namespace `pnam`. The headers under `Source/` are the
**fixed contract**: implement the `.cpp` files against them without changing
signatures.

## 1. Module diagram

```
                          AUDIO THREAD (device callback)
  ┌─────────────────────────────────────────────────────────────────────┐
  │  AudioEngine (owns callback; glue, implemented per §5)              │
  │                                                                     │
  │  PinkNoiseGenerator ──processBlock──► output buffers                │
  │        │ writes post-gain block                                     │
  │        ▼                                                            │
  │  ReferenceHistory (ring, prealloc)                                  │
  │        ▲ readLatest                                                 │
  │        │                                                            │
  │  input buffers ──► AnalysisProcessor::processBlock ──► BlockStats   │
  │                        │ every interval        │                    │
  │                        ▼                       ▼                    │
  │                 MeasurementQueue        EventDetector               │
  │                 (SPSC AbstractFifo)          │ start/end events     │
  │                                              ▼                     │
  │                                         EventQueue (SPSC)          │
  └──────────────────────────┬──────────────────────┬──────────────────┘
                             │ lock-free FIFOs      │
  ┌──────────────────────────▼──────────────────────▼──────────────────┐
  │  MESSAGE THREAD — juce::Timer tick (~30 Hz) drains both queues     │
  │                                                                    │
  │   ├─► UI: meters (also poll AnalysisProcessor atomics), waveform,  │
  │   │      live event log window                                     │
  │   ├─► SessionLogger ──► CsvWriter ×2 ──► measurements.csv /        │
  │   │        (only while logging session active)      events.csv     │
  │   └─► NotificationEngine.postCritical (Critical events only,       │
  │            rate-limited) ──► macOS Notification Center             │
  └────────────────────────────────────────────────────────────────────┘
```

## 2. Files (the contract)

| File | Class / contents |
|---|---|
| `Source/Model/DataModel.h` | `EventType`, `Severity`, `ChannelId`, `StatusBits`, `Measurement`, `Event`, `SessionInfo`, `toString()` helpers |
| `Source/Core/MeasurementQueue.h` | `SpscQueue<T>` (header-only, fully implemented), `MeasurementQueue`, `EventQueue` |
| `Source/Core/PinkNoiseGenerator.h` | `PinkNoiseGenerator` (xorshift64* + Paul Kellet economy pink filter) |
| `Source/Core/ReferenceHistory.h` | `ReferenceHistory` (preallocated power-of-two ring, per channel) |
| `Source/Core/AnalysisProcessor.h` | `AnalysisProcessor`, `BlockStats` |
| `Source/Core/EventDetector.h` | `EventDetector`, `EventDetector::Thresholds` |
| `Source/Logging/CsvWriter.h` | `CsvWriter` (RAII, RFC-4180 quoting, message thread only) |
| `Source/Logging/SessionLogger.h` | `SessionLogger` (session folder + two CSVs + descriptions) |
| `Source/Notifications/NotificationEngine.h` | `NotificationEngine` (per-type cooldown + global cap) |

JUCE modules assumed available: juce_core, juce_audio_basics,
juce_audio_devices, juce_audio_formats, juce_audio_utils, juce_dsp,
juce_gui_basics, juce_gui_extra.

## 3. Thread model (the one rule that matters)

**Audio thread** (`audioDeviceIOCallbackWithContext`):
generate noise → retain reference → analyse input → detect events →
`push()` PODs into the two `SpscQueue`s. It never allocates, locks, does
file I/O, posts notifications, or touches UI. `push()` drops on full
(counted via `getTotalPushFailures()`), never blocks.

**Message thread**: one `juce::Timer` (~30 Hz) owned by the UI calls
`popAll()` on both queues each tick. Each drained `Measurement` updates
meters/waveform and (if a logging session is active) goes to
`SessionLogger::logMeasurement`. Each drained `Event` gets a description via
`SessionLogger::composeDescription`, feeds the live log window,
`SessionLogger::logEvent`, and — if `Severity::Critical` —
`NotificationEngine::postCritical` (rate-limited per type + global cap).
Smooth meter animation additionally polls `AnalysisProcessor`'s atomic
getters directly (no queue round-trip needed).

**Clocking**: the audio thread stamps records with `elapsedSeconds`
(samples processed ÷ sample rate). `SessionLogger` converts to wall-clock
UTC as `SessionInfo::startTimeUtc + elapsedSeconds`.

## 4. Per-callback sequence (audio thread)

1. `PinkNoiseGenerator::processBlock(out, numOutCh, n)` — deterministic
   stream × smoothed gain (level −20 dBFS default, fade, mute); writes the
   post-gain block into the attached `ReferenceHistory`.
2. Build `statusBits` (outputMuted / fadingIn / fadingOut / settling /
   inputInactive) from generator + engine state.
3. `stats = AnalysisProcessor::processBlock(in, numInCh, n, refHistory,
   statusBits)` — per-block peak/RMS/gain-diff/clip count; publishes meter
   atomics; every interval (default 1 s) pushes a `Measurement`.
4. `dropoutActive = EventDetector::processBlockStats(stats)` — state-machine
   detections (dropout / clipping / level change), start+end events only,
   pushed as `Event`s. If true, call `AnalysisProcessor::noteDropoutActive()`.

Detection is suppressed (and open conditions are closed) while any
suppression bit is set in `stats.statusCode`, so fades/mute/startup never
produce false dropouts.

## 5. AudioEngine (glue — no header in the contract; implementer note)

`AudioEngine` owns: `juce::AudioDeviceManager`, the `AudioIODeviceCallback`,
`PinkNoiseGenerator`, `ReferenceHistory`, `AnalysisProcessor`,
`EventDetector`, and both queues (constructed with
`defaultMeasurementQueueCapacity` / `defaultEventQueueCapacity`).
Responsibilities: device selection (mono/stereo in & out), calling
`prepare()` on all core objects in `audioDeviceAboutToStart`, running the §4
sequence in the callback, fading out before stop (`startFade(false)`, wait
for `isFadeActive()==false`, then stop), and pushing an
`EventType::AudioDeviceLost` event from `audioDeviceStopped`/error paths
(message-thread side is fine for these). "Start/Stop Audio" controls the
engine; "Start/Stop Logging" controls only `SessionLogger` — the queues are
drained for the live UI either way.

`ReferenceHistory::prepare` sizing: 2 s at the device rate
(`(int) (sampleRate * 2.0)`), 1 or 2 channels to match the output config.

## 6. Data model / CSV contract

- **Measurement** (audio→message FIFO, trivially copyable):
  `elapsedSeconds, sampleRate, bufferSize, refPeakDb, refRmsDb, inPeakDbL,
  inPeakDbR, inRmsDbL, inRmsDbR, gainDiffDb, correlation (0.0 in V1),
  dropoutFlag, clipCountBlock, statusCode`. Mono sessions mirror L into R.
  Silence = `silenceFloorDb` (−120 dB).
- **Event** (audio→message FIFO, trivially copyable): `type, severity,
  channel, elapsedSeconds, measuredValue, expectedValue, difference,
  thresholdUsed, code` where `code` ∈ {`eventCodeInstant`, `eventCodeStart`,
  `eventCodeEnd`}; end events carry duration (s) in `measuredValue`.
  Description text is composed later on the message thread.
- **SessionInfo** (message thread only, JUCE types allowed): sessionId,
  startTimeUtc, sampleRate, bufferSize, in/out channel counts, device names,
  pinkNoiseSeed, outputLevelDb, logFolder.
- **Session folder**: `<base>/<sessionId>/measurements.csv`, `events.csv`,
  `session_info.txt`. Exact CSV column lists are fixed in `SessionLogger.h`
  (`getMeasurementCsvHeader` / `getEventCsvHeader`).

## 7. V1 → spec mapping

| Spec item | Where |
|---|---|
| Deterministic seeded pink noise, level dBFS, fade, mute | `PinkNoiseGenerator` (xorshift64* + Kellet, default −20 dBFS) |
| Mono / stereo | `StereoMode` + channel counts in `SessionInfo` / `prepare()` |
| Peak+RMS meters (ref + input) | `AnalysisProcessor` atomic getters + `Measurement` |
| Waveform display | UI reads `ReferenceHistory` (ref) + its own input copy from drained data |
| Start/Stop Audio vs Logging | AudioEngine vs `SessionLogger::start/stopSession` |
| CSV measurement + event logs, per-session folder | `SessionLogger` + `CsvWriter` |
| Dropout / clipping / level-change detection | `EventDetector` (state machines, start+end events, debounced) |
| Live event log window | message-thread drain of `EventQueue` + `composeDescription` |
| Desktop notifications, rate-limited | `NotificationEngine` (per-type cooldown 60 s + global 6/min) |
| Audio thread never does I/O/UI | `SpscQueue` over `juce::AbstractFifo`, preallocated PODs |

Out of scope for V1 (fields/hooks reserved): FFT analysis, delay-compensated
correlation (`Measurement::correlation` = 0.0), compression detection, VST.
