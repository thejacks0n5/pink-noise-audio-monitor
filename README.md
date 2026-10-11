# Pink Noise Audio Monitor

An unattended test-monitoring tool for macOS: it plays a deterministic pink
noise reference out of an audio interface, listens to what comes back, and
continuously compares and logs the two, so you can find dropouts, clipping,
level changes, clicks, noise, distortion and timing slips in an audio path
over runs that last hours or days. It also shows the path's round-trip delay
live.

Current version: 1.2.0. Installed copies (1.0.2 or later) check the Releases
page and offer to update themselves; nothing installs without asking.

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

## What it detects

Level faults, judged against the level the reference predicts: **dropouts**,
**clipping** and **level changes**. And artifacts, judged by comparing the
return sample by sample with the exact noise that was sent: **clicks and pops**,
**noise** (static, hiss, crackle, buzz), **distortion** (clipping or saturation
somewhere in the path), **timing slips** (dropped or repeated samples, with the
count) and **signal mismatch** (the return is not the signal that was sent).
`measurements.csv` also records each input's round-trip latency and how much of
the return is not the reference.

**Live delay monitor.** The DELAY strip shows each input's round-trip delay as
it runs. Cable the output straight to the input, press Calibrate to zero out
the interface's own latency, then put a device or system in the path to read
its delay in milliseconds and samples (up to about 0.68 s). `BUILD_NOTES.md` §8.

Limits worth knowing before trusting a clean log: pure mains hum below about
200 Hz is not reliably reported, and a compressor or limiter in the path makes
artifact detection unusable (switch it off in Settings). `BUILD_NOTES.md` §7
has the full list and what has and has not been verified on real hardware.

## Tests

```sh
cmake --build build --target pnam_tests -j && ./build/pnam_tests_artefacts/Release/pnam_tests
```

Runs the real generator through a simulated path into the real artifact
detector, injecting known faults (about three minutes in a Release build).

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
3. In the app: click **Audio Setup**, select the devices above and the
   channels that are actually cabled, untick Monitor on any input with nothing
   patched in, then click **Start Audio**. The reference and input meters and
   waveform lanes should move together, and within a few seconds the DELAY
   strip should show the round-trip delay. Click **Start Logging** to begin
   writing CSVs. Pulling the input cable should log one dropout start and,
   on reconnecting, one end.
4. Logging needs a named log destination first: Logging menu → **Set Log
   File...** (Cmd-Shift-L). Each session writes a folder holding
   `measurements.csv`, `events.csv` and `session_info.txt`.

## Notes

- Where to read more: `CLAUDE.md` for the current architecture and rules;
  `BUILD_NOTES.md` for the full record (what was done, every decision and
  its reason, review findings, open items, reference tables);
  `ARCHITECTURE.md` and `UI_DESIGN.md` for the original V1 design;
  `docs/design/` for the icon and theme.
- Settings are stored per machine in
  `~/Library/Application Support/PinkNoiseAudioMonitor/`.
- `CMakeLists.txt` globs all `Source/**/*.cpp` automatically, so newly added
  `.cpp` files are picked up on the next `cmake` configure/build without
  editing the build script.
