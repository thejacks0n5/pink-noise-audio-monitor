# Design sources: icon and theme

The record of how the app's icon and colour theme were made (October 2026).
What ships is in `Resources/` (the compiled icon catalog and its source PNGs)
and `Source/UI/DarkLookAndFeel.h` (the palette). This folder is the reasoning
and the sources behind them.

| File | What it is |
|---|---|
| `DESIGN_NOTES.md` | The designer's notes: the three icon concepts, why A was recommended, each revision, known weaknesses. |
| `theme.md` | One row per `Palette` entry with its value and reason; every contrast ratio; colour-vision tables; per-channel trace colours; limits. Matches the shipped blue palette. |
| `INTEGRATION.md` | How the icon catalog goes into the bundle, and the paste-ready palette block. |
| `AUDIT.md` | The independent audit. **It was written against the first (pink) delivery**, before the body shape, ruler, packaging and colour were changed; its required changes were all applied afterwards. Its theme findings that did not depend on the accent colour still stand. |
| `icon-masters/` | SVG masters for icon A: the 1024 master, and the hand-fitted 16 and 32 px versions. |
| `tools/` | The scripts that generated the icons, sheets, mock and theme tables. They were written to run in the designer's own working folder and are kept as a record; expect to adjust paths before running them. |
| `icon_contact_sheet.png` | The first round: concepts A, B and C, in pink. Kept as the record of the concept choice. |
| `blue_options.png` | Icon A in the three blues offered. Option 3 (`#3391FF`) was chosen. |
| `theme_preview.png`, `theme_states.png` | Drawn mock of the main window, and swatch/state sheets, in the shipped palette. The mock predates the DELAY strip. |

Not kept (they lived in a temporary folder): the per-size PNG renders of the
rejected concepts, the auditor's probe bundles and comparison renders, and the
system-render verification sheets. `BUILD_NOTES.md` §10.8 and §11.6 summarise
what they showed.
