# Pink Noise Audio Monitor (Version One)

An overnight test-monitoring tool for macOS: generates a deterministic pink
noise reference signal, plays it out through a chosen output device, listens
to the corresponding input device, and continuously analyses/logs the two so
you can detect dropouts, clipping, level drift, and correlation loss over long
unattended runs.

This README covers building and running the **Version One foundation** — the
scaffolding, core DSP/analysis engine, logging, and UI are all present; this
is the base you build and iterate on from here.

## Prerequisites

- macOS 12 (Monterey) or later, Apple Silicon.
- Xcode (full app, not just the command line tools) — the CMake build below
  generates an Xcode project and needs the Xcode toolchain to compile it.
  Install/update from the App Store, then run once:
  ```sh
  xcode-select --install
  sudo xcodebuild -license accept
  ```
- CMake 3.22 or later:
  ```sh
  brew install cmake
  ```
- A network connection the first time you configure — CMake's
  `FetchContent` pulls the JUCE framework (tag `8.0.4`) directly from GitHub
  into `build/_deps/`.

## Building

From the project root (the directory containing `CMakeLists.txt`):

```sh
# 1. Configure — generates an Xcode project under build/ and fetches JUCE
cmake -B build -G Xcode -DCMAKE_BUILD_TYPE=Release

# 2. Build from the command line...
cmake --build build --config Release

# ...or open the generated Xcode project and build/run from the IDE:
open build/PinkNoiseAudioMonitor.xcodeproj
```

The first configure step will take a few minutes while JUCE is cloned. It
only needs to happen once (or again if you delete `build/`).

If you prefer a plain Makefile/Ninja build instead of an Xcode project
(useful for quick CLI iteration, though you'll want Xcode for signing/
notarizing later):

```sh
cmake -B build-cli -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build-cli --config Release
```

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
4. Session folders (CSV logs) are written under
   `~/Documents/PinkNoiseAudioMonitor/<session-id>/` by default — configurable
   from the Settings dialog.

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
