# Pink Noise Audio Monitor — V1 Core Architecture

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
