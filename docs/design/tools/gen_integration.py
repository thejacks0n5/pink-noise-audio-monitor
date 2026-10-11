#!/usr/bin/env python3
"""Writes final/INTEGRATION.md. The palette block and file facts are read from
palette.py and from the files in final/, not typed in."""
import hashlib, os, plistlib, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from palette import PALETTE, PREVIOUS

F = os.path.join(ROOT, "final")
def info(name):
    p = os.path.join(F, name); d = open(p, "rb").read()
    return len(d), hashlib.sha256(d).hexdigest()[:16]
pl = plistlib.load(open(os.path.join(F, "AppIcon-partial-Info.plist"), "rb"))
icon_name, icon_file = pl["CFBundleIconName"], pl["CFBundleIconFile"]

GROUPS = [("Surfaces", 8), ("Text", 4), ("Accent", 3), ("Meters / signal", 6), ("Waveform traces", 5), ("Severity / status", 6)]
block = ["%-18s = 0x%08x" % (name, new) for name, cur, new in PALETTE]
assert len(block) == 32 and sum(n for _, n in GROUPS) == 32
changed = sum(1 for n, c, v in PALETTE if c != v)

L = []; A = L.append
A("# Integration: icon and palette\n")
A("Nothing in the repo has been changed. This lists what to copy, where it goes in the built app, and the values to paste. "
  "The icon half was verified on this Mac (macOS 27.0) by asking the system to draw it; none of it has been tried in a real build of the app.\n")
A("**This is the blue revision.** The icon band and the theme accent are blue `#3391FF`; an earlier copy of this file "
  "described a pink icon and a pink accent and is replaced by this one. The body shape, ruler, packaging and verification "
  "method are the ones fixed after the audit.\n")
A("## 1. Icon files\n")
A("| File in `design/final/` | Size | sha256 (first 16) | Where it goes in the app bundle |")
A("|---|---|---|---|")
for name, dest in (("Assets.car", "`Pink Noise Audio Monitor.app/Contents/Resources/Assets.car`"),
                   ("AppIcon.icns", "`Pink Noise Audio Monitor.app/Contents/Resources/AppIcon.icns`")):
    n, h = info(name)
    A("| `%s` | %d bytes | `%s` | %s |" % (name, n, h, dest))
A("")
A("Both files go in `Contents/Resources/`, under exactly those names. `Assets.car` is the icon the system uses. "
  "`AppIcon.icns` is the fallback `actool` writes alongside it; it holds only 16, 32, 128 and 256 px entries and is there for "
  "anything that does not read asset catalogs. Ship both, but see the warning in section 3.\n")
A("Also in `design/final/`, for the repo rather than the bundle:\n")
A("- `Assets.xcassets/` is the source of `Assets.car` (ten PNGs and two `Contents.json`). Keep it in the repo, for example at "
  "`Resources/Icon/Assets.xcassets`, so the catalog can be rebuilt. It does not go into the bundle.")
A("- `AppIcon-partial-Info.plist` is `actool`'s record of the two plist keys below. Reference only.")
A("- `icon_1024.png` is the 1024 px render, for a README or a disk image. Not needed in the bundle.\n")
A("To rebuild `Assets.car` and `AppIcon.icns` from the catalog (this is the command that produced them):\n")
A("```sh\nxcrun actool Assets.xcassets --compile <output-folder> --platform macosx \\\n"
  "    --minimum-deployment-target 12.0 --app-icon AppIcon \\\n"
  "    --output-partial-info-plist <output-folder>/AppIcon-partial-Info.plist\n```\n")
A("## 2. Info.plist keys\n")
A("| Key | Value |")
A("|---|---|")
A("| `CFBundleIconName` | `%s` |" % icon_name)
A("| `CFBundleIconFile` | `%s` |" % icon_file)
A("")
A("Both are strings; set both, with the values `actool` emitted (no file extension). `CFBundleIconName` is what makes the "
  "system read the icon from `Assets.car`; `CFBundleIconFile` names the fallback `.icns`.\n")
A("### One way to do it in this project's CMake (untested)\n")
A("`CMakeLists.txt` currently passes `ICON_BIG \"\"` and `ICON_SMALL \"\"` to `juce_add_gui_app`. Leave those empty: given a PNG, "
  "JUCE builds its own `.icns` and the hand-fitted 16 and 32 px artwork is lost. With them empty JUCE writes an empty "
  "`CFBundleIconFile`; from reading JUCE 8.0.4's `juce_PlistOptions.cpp`, keys supplied through `PLIST_TO_MERGE` are kept "
  "in preference to the generated ones. So, assuming the two files are copied to `Resources/Icon/` in the repo:\n")
A("```cmake\njuce_add_gui_app(PinkNoiseAudioMonitor\n    # ... existing arguments unchanged ...\n"
  "    PLIST_TO_MERGE \"<plist version=\\\"1.0\\\"><dict><key>CFBundleIconName</key><string>%s</string><key>CFBundleIconFile</key><string>%s</string></dict></plist>\"\n)\n\n"
  "set(PNAM_ICON_FILES\n    \"${CMAKE_CURRENT_SOURCE_DIR}/Resources/Icon/Assets.car\"\n    \"${CMAKE_CURRENT_SOURCE_DIR}/Resources/Icon/AppIcon.icns\")\n"
  "target_sources(PinkNoiseAudioMonitor PRIVATE ${PNAM_ICON_FILES})\n"
  "set_source_files_properties(${PNAM_ICON_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)\n```\n" % (icon_name, icon_file))
A("I have not run this. Check the built bundle for the two files in `Contents/Resources/` and the two keys in "
  "`Contents/Info.plist` (`plutil -p`). The files must be in place before the bundle is signed and notarized.\n")
A("## 3. What was verified, and what was not\n")
A("Verified here, by `tools/verify_icon.sh`: `Assets.car` and `AppIcon.icns` were put in a throwaway bundle with the two keys "
  "above, and `NSWorkspace.icon(forFile:)` was drawn at 16, 32, 64, 128, 256, 512 and 1024 px. Renders are in "
  "`design/verify/system_renders/`; inspection sheets are `design/verify/system_all_sizes.png` and `system_small_zoom.png`. "
  "At every size the artwork is full-bleed with no plate, and the amber mark is present at 16 px.\n")
A("**Warning.** The same bundle with only `AppIcon.icns` and no `CFBundleIconName` (the control in "
  "`design/verify/icns_only_renders/`) is drawn shrunk onto a black plate at 16 px on this OS. If the catalog is not picked up, "
  "that is what a Finder list view will show. So after the first real build, look at the app at small sizes, not only in the Dock.\n")
A("Not verified: the real app bundle; the Dock, a Finder list view and Get Info on the target Mac mini; any macOS older than "
  "27.0 (the catalog was compiled for 12.0 and later but only drawn on 27.0); the light, tinted and clear icon appearances. "
  "The very first request for a new bundle's icon can return a generic dashed placeholder for a moment while the system renders it; "
  "that is normal and is why the verification script asks twice.\n")
A("## 4. Palette\n")
A("All 32 entries of `pnam::Palette` in `Source/UI/DarkLookAndFeel.h`, in the order they are declared there (surfaces, text, "
  "accent, meters, traces, severity and status). %d of the 32 values differ from what ships (all of them, if that reads 32); none is added or removed. Each line replaces the value on the matching `static constexpr juce::uint32` line. "
  "`meterAmberFromDb` and `meterRedFromDb` are not colours and stay as they are.\n" % changed)
A("```\n" + "\n".join(block) + "\n```\n")
A("Compared with the pink palette in the previous copy of this file, %d entries changed: %s. The other %d are the same.\n"
  % (len(PREVIOUS), ", ".join("`%s`" % n for n, c, v in PALETTE if n in PREVIOUS), len(PALETTE) - len(PREVIOUS)))
A("Text that the new values make wrong, to fix in the same change:\n")
A("- `DarkLookAndFeel.h`: the banner comment (\"desaturated steel-blue accent\"), the `// -- Accent (desaturated steel blue)` "
  "heading, and the trailing `// accent`, `// green`, `// muted violet` comments on the three trace entries.")
A("- `UI_DESIGN.md` section 2: the table of hex values.\n")
A("Reasons for each value, contrast figures and known limits are in `design/theme.md`.\n")
open(os.path.join(F, "INTEGRATION.md"), "w").write("\n".join(L))
print("wrote final/INTEGRATION.md; palette values changed:", changed)
