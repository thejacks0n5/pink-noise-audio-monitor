# Pink Noise Audio Monitor (Version One)

An overnight test-monitoring tool for macOS: generates a deterministic pink
noise reference signal, plays it out through a chosen output device, listens
to the corresponding input device, and continuously analyses/logs the two so
you can detect dropouts, clipping, level drift, and correlation loss over long
unattended runs.

This README covers building and running the **Version One foundation** — the
scaffolding, core DSP/analysis engine, logging, and UI are all present; this
is the base you build and iterate on from here.

## Quick start on another Mac

Runs on macOS 12 or later, Apple Silicon or Intel (the release build is universal).

**Option A — download the built app.** Grab
`PinkNoiseAudioMonitor-1.0.0-macOS-universal.zip` from the repo's
[Releases](https://github.com/thejacks0n5/pink-noise-audio-monitor/releases)
page, unzip it and open it. The release build is signed with a Developer ID
and notarized, so Gatekeeper opens it without any extra step. Allow microphone
access when prompted, or every input reads silence.

**Option B — build from source.** Needs the Xcode Command Line Tools
(`xcode-select --install`), CMake 3.22+ (`brew install cmake`) and a network
connection for the first configure, which fetches JUCE `8.0.4` into
`build/_deps/`. Full Xcode is not required.

```sh
git clone https://github.com/thejacks0n5/pink-noise-audio-monitor.git
cd pink-noise-audio-monitor
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build -j
open "build/PinkNoiseAudioMonitor_artefacts/Release/Pink Noise Audio Monitor.app"
```

A source build is ad-hoc signed, which is fine on the machine that built it.
Without `CMAKE_OSX_ARCHITECTURES` the build is arm64 only, which will not run
on an Intel Mac. The first build takes a few minutes.

Settings do not travel between machines. On a new one, follow
`BUILD_NOTES.md` §5: pick the interface and channel counts, name the channels,
untick Monitor on unpatched inputs, set the log file (Logging menu), grant the
microphone permission, and stop the machine sleeping.

To generate an Xcode project instead (needs full Xcode with its license
accepted): `cmake -B build -G Xcode`, then `cmake --build build --config Release`.

## Where the app lands

After a successful build, the app bundle is at:

```
build/PinkNoiseAudioMonitor_artefacts/Release/Pink Noise Audio Monitor.app
```

(For a Debug configuration, substitute `Debug` for `Release` in that path.)

Launch it directly:

```sh
open "build/PinkNoiseAudioMonitor_artefacts/Release/Pink Noise Audio Monitor.app"
```

On first launch, macOS will prompt for microphone access (required to read
the input device) — grant it under System Settings → Privacy & Security →
Microphone if you miss the prompt.

## Running the loopback test

The simplest way to validate the whole signal path end-to-end is a physical
or virtual loopback so the app's own pink noise output becomes its input:

1. **Physical loopback (simplest, any Mac):** connect a cable from a
   headphone/line output to a line input (or use a USB audio interface with
   a hardware loopback/monitor path). Set your system output and the app's
   output device to that interface's output, and the app's input device to
   the corresponding input.
2. **Virtual loopback (no extra hardware):** install a virtual audio device
   such as [BlackHole](https://github.com/ExistentialAudio/BlackHole) (2ch),
   then:
   - Set the app's **output device** to BlackHole 2ch.
   - Set the app's **input device** to BlackHole 2ch.
   - Optionally create a Multi-Output Device in Audio MIDI Setup if you also
     want to hear the pink noise while it's routed through BlackHole.
3. In the app: open device setup from the transport bar, select the devices
   above, click **Start Audio**. You should see the reference and input
   meters and waveform track each other closely, `gainDiffDb` sit near 0 dB,
   and correlation read near 1.0. Click **Start Logging** to begin writing
   CSVs; try a brief mute or a disconnected input to confirm dropout/clipping
   events show up in the event log and get written to `events.csv`.
4. Logging needs a named log destination first: Logging menu → **Set Log
   File...** (Cmd-Shift-L). Each session writes a folder holding
   `measurements.csv`, `events.csv` and `session_info.txt`.

## Notes

- This is the **Version One** foundation: core generator/analysis/event
  detection, CSV session logging, local critical-event notifications, and the
  full dark-themed UI are implemented against the fixed header contracts in
  `Source/Model`, `Source/Core`, `Source/Logging`, `Source/Notifications`, and
  `Source/UI`. `Source/Core/AudioEngine.h/.cpp` (the device-manager glue that
  wires generator → reference history → analysis → event detector each audio
  callback) is the integration layer described in `ARCHITECTURE.md` §5.
- `CMakeLists.txt` globs all `Source/**/*.cpp` automatically, so newly added
  `.cpp` files are picked up on the next `cmake` configure/build without
  editing the build script.
