#!/bin/sh
# Rebuilds every generated file in this folder from the scripts.
# Needs python3 (stdlib), Google Chrome (headless, for rasterising), swift and
# Xcode's actool (xcrun). The shipping icon is assembled in final/.
set -e
cd "$(dirname "$0")/.."
python3 tools/gen_icons.py                 # SVG masters and PNGs: the three blue candidates and the final icon A
python3 tools/gen_blue_options.py          # blue_options.png, extras/blue_options_zoom.png
# icon_contact_sheet.png and extras/zoom_small.png are the first-round A/B/C comparison in the
# original pink and are deliberately NOT rebuilt here (tools/gen_sheets.py made them; B, C and
# the pink A can be regenerated with: python3 tools/gen_icons.py A_pink B C).
cp icons/A/A_1024.png icon_1024.png
./tools/build_final.sh                     # final/Assets.xcassets -> Assets.car, AppIcon.icns, partial plist
./tools/verify_icon.sh                     # dummy bundles, system renders at 16..1024 px
python3 tools/verify_sheet.py              # band measurements + inspection sheets in verify/
python3 tools/gen_theme_docs.py            # theme.md, theme_states.png
python3 tools/gen_mock.py                  # theme_preview.svg/.png
python3 tools/gen_mock.py current          # extras/: same mock in the shipping palette
python3 tools/gen_integration.py           # final/INTEGRATION.md
rm -rf tools/__pycache__ extras/*.html
