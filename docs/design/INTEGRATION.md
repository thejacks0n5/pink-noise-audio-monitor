# Integration: icon and palette

Nothing in the repo has been changed. This lists what to copy, where it goes in the built app, and the values to paste. The icon half was verified on this Mac (macOS 27.0) by asking the system to draw it; none of it has been tried in a real build of the app.

**This is the blue revision.** The icon band and the theme accent are blue `#3391FF`; an earlier copy of this file described a pink icon and a pink accent and is replaced by this one. The body shape, ruler, packaging and verification method are the ones fixed after the audit.

## 1. Icon files

| File in `design/final/` | Size | sha256 (first 16) | Where it goes in the app bundle |
|---|---|---|---|
| `Assets.car` | 299336 bytes | `8a3d69f350334316` | `Pink Noise Audio Monitor.app/Contents/Resources/Assets.car` |
| `AppIcon.icns` | 37728 bytes | `045232ad9b68845d` | `Pink Noise Audio Monitor.app/Contents/Resources/AppIcon.icns` |

Both files go in `Contents/Resources/`, under exactly those names. `Assets.car` is the icon the system uses. `AppIcon.icns` is the fallback `actool` writes alongside it; it holds only 16, 32, 128 and 256 px entries and is there for anything that does not read asset catalogs. Ship both, but see the warning in section 3.

Also in `design/final/`, for the repo rather than the bundle:

- `Assets.xcassets/` is the source of `Assets.car` (ten PNGs and two `Contents.json`). Keep it in the repo, for example at `Resources/Icon/Assets.xcassets`, so the catalog can be rebuilt. It does not go into the bundle.
- `AppIcon-partial-Info.plist` is `actool`'s record of the two plist keys below. Reference only.
- `icon_1024.png` is the 1024 px render, for a README or a disk image. Not needed in the bundle.

To rebuild `Assets.car` and `AppIcon.icns` from the catalog (this is the command that produced them):

```sh
xcrun actool Assets.xcassets --compile <output-folder> --platform macosx \
    --minimum-deployment-target 12.0 --app-icon AppIcon \
    --output-partial-info-plist <output-folder>/AppIcon-partial-Info.plist
```

## 2. Info.plist keys

| Key | Value |
|---|---|
| `CFBundleIconName` | `AppIcon` |
| `CFBundleIconFile` | `AppIcon` |

Both are strings; set both, with the values `actool` emitted (no file extension). `CFBundleIconName` is what makes the system read the icon from `Assets.car`; `CFBundleIconFile` names the fallback `.icns`.

### One way to do it in this project's CMake (untested)

`CMakeLists.txt` currently passes `ICON_BIG ""` and `ICON_SMALL ""` to `juce_add_gui_app`. Leave those empty: given a PNG, JUCE builds its own `.icns` and the hand-fitted 16 and 32 px artwork is lost. With them empty JUCE writes an empty `CFBundleIconFile`; from reading JUCE 8.0.4's `juce_PlistOptions.cpp`, keys supplied through `PLIST_TO_MERGE` are kept in preference to the generated ones. So, assuming the two files are copied to `Resources/Icon/` in the repo:

```cmake
juce_add_gui_app(PinkNoiseAudioMonitor
    # ... existing arguments unchanged ...
    PLIST_TO_MERGE "<plist version=\"1.0\"><dict><key>CFBundleIconName</key><string>AppIcon</string><key>CFBundleIconFile</key><string>AppIcon</string></dict></plist>"
)

set(PNAM_ICON_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Icon/Assets.car"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Icon/AppIcon.icns")
target_sources(PinkNoiseAudioMonitor PRIVATE ${PNAM_ICON_FILES})
set_source_files_properties(${PNAM_ICON_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
```

I have not run this. Check the built bundle for the two files in `Contents/Resources/` and the two keys in `Contents/Info.plist` (`plutil -p`). The files must be in place before the bundle is signed and notarized.

## 3. What was verified, and what was not

Verified here, by `tools/verify_icon.sh`: `Assets.car` and `AppIcon.icns` were put in a throwaway bundle with the two keys above, and `NSWorkspace.icon(forFile:)` was drawn at 16, 32, 64, 128, 256, 512 and 1024 px. Renders are in `design/verify/system_renders/`; inspection sheets are `design/verify/system_all_sizes.png` and `system_small_zoom.png`. At every size the artwork is full-bleed with no plate, and the amber mark is present at 16 px.

**Warning.** The same bundle with only `AppIcon.icns` and no `CFBundleIconName` (the control in `design/verify/icns_only_renders/`) is drawn shrunk onto a black plate at 16 px on this OS. If the catalog is not picked up, that is what a Finder list view will show. So after the first real build, look at the app at small sizes, not only in the Dock.

Not verified: the real app bundle; the Dock, a Finder list view and Get Info on the target Mac mini; any macOS older than 27.0 (the catalog was compiled for 12.0 and later but only drawn on 27.0); the light, tinted and clear icon appearances. The very first request for a new bundle's icon can return a generic dashed placeholder for a moment while the system renders it; that is normal and is why the verification script asks twice.

## 4. Palette

All 32 entries of `pnam::Palette` in `Source/UI/DarkLookAndFeel.h`, in the order they are declared there (surfaces, text, accent, meters, traces, severity and status). 32 of the 32 values differ from what ships (all of them, if that reads 32); none is added or removed. Each line replaces the value on the matching `static constexpr juce::uint32` line. `meterAmberFromDb` and `meterRedFromDb` are not colours and stay as they are.

```
windowBackground   = 0xff0a0b0d
panelBackground    = 0xff15171b
panelBackgroundAlt = 0xff1a1c21
controlBackground  = 0xff23262d
controlHover       = 0xff2b2e37
controlPressed     = 0xff363a45
outline            = 0xff353942
outlineFocused     = 0xff55a4ff
textPrimary        = 0xffe8eaee
textSecondary      = 0xffa4aab5
textDisabled       = 0xff6e7480
textOnAccent       = 0xff050d1a
accent             = 0xff3391ff
accentDim          = 0xff3585e6
accentBright       = 0xff55a4ff
meterGreen         = 0xff3bd1a9
meterAmber         = 0xffffd452
meterRed           = 0xffff6450
meterTrough        = 0xff0e0f12
meterPeakTick      = 0xfff6f7f9
meterRmsOverlay    = 0x4dffffff
traceReference     = 0xffcfd4e6
traceInputL        = 0xffc7eb75
traceInputR        = 0xffa8eb75
traceGrid          = 0xff1c1e24
traceCentreLine    = 0xff2d3039
severityInfo       = 0xffbcc2cd
severityWarning    = 0xffffd452
severityCritical   = 0xffff6450
statusRunning      = 0xff3bd1a9
statusIdle         = 0xff555a65
statusPaused       = 0xffffd452
```

Compared with the pink palette in the previous copy of this file, 9 entries changed: `outlineFocused`, `textOnAccent`, `accent`, `accentDim`, `accentBright`, `traceReference`, `traceInputL`, `traceInputR`, `severityInfo`. The other 23 are the same.

Text that the new values make wrong, to fix in the same change:

- `DarkLookAndFeel.h`: the banner comment ("desaturated steel-blue accent"), the `// -- Accent (desaturated steel blue)` heading, and the trailing `// accent`, `// green`, `// muted violet` comments on the three trace entries.
- `UI_DESIGN.md` section 2: the table of hex values.

Reasons for each value, contrast figures and known limits are in `design/theme.md`.
