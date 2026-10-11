# Audit: icon and theme deliverables for Pink Noise Audio Monitor

Audited 2026-10-10 on this Mac (macOS 27.0, build 26A428). Nothing in `design/` or the repo was modified. Everything I generated is in this folder. The app was not built or launched (other people are editing the tree); the theme findings are computed from the source and the proposed values, and the icon findings are from files plus what this Mac's icon services actually render.

## Verdict

**Ship with changes.**

- **Icon: concept A is the right choice, but the icon files as delivered must not ship.** On this Mac's OS the delivered `AppIcon.icns` is drawn shrunk onto a black plate at 16, 32, 512 and 1024 px. Two causes, both fixable, both verified by experiment (section 1).
- **Theme: the 32 values can ship as they are.** Every value is valid, the mapping is exactly one-to-one, the contrast table is right to within rounding, and no value has to change. There are optional improvements and some code-level follow-ups (sections 3 to 5).

## 1. Icon files

### What checks out

| Claim | Result | Evidence |
|---|---|---|
| `icon_1024.png` is 1024x1024 | True | `sips`: 1024x1024, 8-bit RGBA. Byte-identical (md5) to `icons/A/A_1024.png` and `AppIcon.iconset/icon_512x512@2x.png`. |
| Transparent outside the body | True, apart from the disclosed baked shadow | All four corners alpha 0. Opaque body is exactly x,y 100..923 (824 px). 68,631 partial-alpha pixels are the drop shadow, which reaches y=957 at alpha 27/255 or less. |
| `.icns` has the full set | True | Ten entries: ic04 (16), ic11 (16@2x), ic05 (32), ic12 (32@2x), ic07, ic13, ic08, ic14, ic09, ic10. Extracted with `iconutil -c iconset` into `extracted.iconset/`; sizes 16, 32, 32, 64, 128, 256, 256, 512, 512, 1024. |
| `.icns` content matches the PNGs | True | 64 px and up are pixel-identical after extraction. The 16 and 32 px (1x) entries show 24 and 52 differing edge pixels after `iconutil` extraction; I decoded the `ic04` entry by hand and the stored bytes equal the designer's PNG, so that is an extraction artefact, not a defect. |
| Body size matches neighbours | True | Every icon this Mac renders (Calculator, Music, Voice Memos, Chrome, Slack, etc. via `NSWorkspace.icon(forFile:)`) has its body at 100..923 of 1024. The designer's is 100..923. At 16 and 32 px the system's own body is 14 px and 26 px, the same as the designer's snapped sizes. |
| Art reaching the body edge is safe | True | At sizes where the system accepts the icon it re-masks to its own rounder corner and adds its own rim; the edge-to-edge band survives cleanly (`sys_render_sheet_256.png`, top left). |

### What does not: the system puts the delivered icon on a plate

I put the delivered `AppIcon.icns` in a dummy bundle (`probe/IconProbe2.app`, never launched) and asked the system for its icon at each size. Result, `probe/out2/`:

| Size requested | Delivered `AppIcon.icns` |
|---|---|
| 16 | Shrunk onto a black plate, hand-fitted pixels resampled to mush |
| 32 | Shrunk onto a black plate |
| 64, 128, 256 | Full-bleed, correct |
| 512, 1024 | Shrunk onto a black plate (art is 54% of the canvas) |

See `sys_vs_designer_small.png` (left: designer's files; right: what macOS draws) and `probe/out2/IconProbe2_1024.png`. This is the "legacy icon" treatment recent macOS gives to icons whose shape it does not recognise. Where it shows: Finder list and column views, sidebar, Spotlight and menus (16 and 32 px, and all 16/32 pt uses on a non-Retina monitor), and Get Info, Quick Look and large Finder icons (512 and 1024). The Dock at normal sizes is unaffected.

I isolated the causes with variant icons built in `exp/`:

| Variant | 16 | 32 | 64-256 | 512 | 1024 |
|---|---|---|---|---|---|
| Delivered `.icns` | plate | plate | ok | plate | plate |
| E1: designer's 1024 master downsampled to all sizes, `.icns` | plate | plate | ok | ok | plate |
| E4: as E1 with the shadow stripped | plate | plate | ok | ok | plate |
| E2: same art, body re-masked to Apple's template shape, `.icns` | plate | plate | ok | ok | ok |
| E6: E2 with only the 128 to 1024 entries in the `.icns` | ok | ok | ok | ok | ok |
| E7: E2 with only the 1024 entry | ok | ok | ok | ok | ok |
| CatDesigner: delivered PNGs in an asset catalog (`Assets.car`) | ok | ok | ok | ok | plate |
| **CatMix: Apple-shape 64 to 1024 plus the designer's hand-fitted 16 and 32, asset catalog** | **ok** | **ok** | **ok** | **ok** | **ok** |

Conclusions:

1. **The superellipse body is the cause at large sizes.** The designer flagged "approximates Apple's shape" as a weakness; it matters. Against Apple's template (824 px, continuous corner radius 185.4, generated with SwiftUI in `exp/mask.swift`) the superellipse matches on the diagonal (both first opaque at 154) but sits up to 3 px inside along the shoulders: 2,876 pixels short at 1024. That is enough to fail the system's shape check. The shadow is irrelevant.
2. **Small entries in a classic `.icns` are the cause at 16 and 32 px.** Even with the exact Apple shape, an `.icns` that contains 16 and 32 px entries gets the plate at those sizes. Another icns-only app on this Mac (Hermes) shows the same behaviour. Leave them out and the system scales the big art down full-bleed; put them in an asset catalog instead and they are used as drawn.
3. **The asset-catalog route preserves the hand-fitted small sizes.** In CatMix the system's 16 px render matches the designer's `A_16.png` within 10 levels on every pixel, and the 32 px within 13 (`catalog_route.png`).

On the designer's last flagged weakness (CMake `ICON_BIG` would discard the hand-fitted versions): I read JUCE 8.0.4's `juce_Icons.cpp`. Given one 1024 PNG it writes an `.icns` with only a 1024 and a 512 entry. That is the E7 situation, so on this OS it renders full-bleed everywhere, which is better than the delivered `.icns`. The cost is real though: the system-scaled 16 px loses the amber mark to an olive smudge (`small_three_way.png`, right column).

## 2. Small-size legibility, and which concept to ship

Looked at `A_16/32`, `B_16/32`, `C_16/32` enlarged on four backgrounds (`my_zoom_16.png`, `my_zoom_32.png`), at true size (`my_truesize.png`), and at 64 and 128 (`mid_sizes_dark.png`).

| | 16 px | 32 px | Misreading risk |
|---|---|---|---|
| **A** | A pink bar across a dark tile with a 2 px break right of centre and a 2x1 amber dash above the break. Crisp. | Two pink blocks joined by a thin pink link, with a small amber staple above the join. | Low. Not a warning sign (no triangle, no exclamation), not "disabled" (no slash or ring), not a broken image. Nearest misreadings: a minus sign or a level bar at 16; at 32 the amber staple in the upper right could pass for a badge. On a near-black Dock the dark body nearly disappears and it reads as a floating pink dash. |
| **B** | Three pink bars and one short white one. The crispest glyph of the three. | Same, with gradient. | Reads as a generic bar chart or signal-strength icon. Nothing says "audio" or "fault". |
| **C** | A black slash on a pink tile. | The slash plus the faint vertical grid lines form a letter **N**. | High. "Disabled" at 16, a letter at 32. |

The designer's ranking (A, then B, then C) is right. **I would ship A.** At Dock sizes it is the only one that is recognisably this tool, and its 16 px version is acceptable provided the hand-fitted file is the one that gets shown, which is what the packaging change above secures. B is the fallback only if the owner wants maximum plainness.

One association to be aware of at large sizes: a yellow bracket around a stretch of waveform is also how Apple draws trim handles, so A can read as "clip trimmer" before it reads as "dropout".

## 3. Theme correctness

Script: `theme_check.py`, output `theme_check.out`. JUCE colour functions were transcribed from the JUCE 8.0.4 source in `build/_deps/juce-src`, not from the designer's tools.

- **Mapping:** 32 entries in the header, 32 proposed, same names, same order, none missing, none added, no duplicates. Every value is eight hex digits; alpha is `ff` everywhere except `meterRmsOverlay` (`2e`). The prose table, the paste-ready block and the header's current values all agree.
- **Contrast table:** all 28 of the designer's rows reproduce. Five differ by more than 0.03 because the designer's re-implementation of JUCE's integer maths is slightly off (largest: pressed button 13.55 stated, 13.67 actual). No pass or fail changes. The trace table matches JUCE exactly, hex for hex.

Every text pairing under threshold. Nothing in the UI is large text by the WCAG definition, so 4.5:1 applies throughout; I list under-3:1 separately because the brief asks.

| Text | On | Where | Ratio | Note |
|---|---|---|---|---|
| `textSecondary` at 50% alpha | `windowBackground` | "(no channel selected)" lane hint, `WaveformDisplay.cpp:368` | **2.88** (also under 3:1) | Designer flagged (stated 2.85). The alpha is in code; removing it gives 8.43. 2.40 today. |
| `textOnAccent` | `accentDim.darker(0.2)` = `#A64F7C` | Latched Mute or Resume while the mouse is held down | **3.76** | Designer flagged (stated 3.78). Lasts as long as a click. 2.13 today. |
| `textDisabled` | `accentDim` | Mute still latched after Stop Audio: the button is disabled and toggled | **1.24** (under 3:1) | **Not in the designer's table.** Disabled controls are exempt from WCAG, but the label is unreadable. From the code this state is reachable (`updateEnablement` disables Mute when audio stops and nothing clears the toggle). 1.00 today. Needs a code change, not a value. |

Everything else passes: the lowest passing body-text pair is `severityCritical` on a selected row at 4.64. The shipping palette fails 9 of the same rows.

Pairings the designer's table omitted that I checked and that pass: `textSecondary` captions inside a hovered status-bar cell (5.81), About-box text on `windowBackground` (8.43 and 16.35), "OFF" over dimmed bars including the dimmed RMS cap (7.40 to 9.71).

Non-text items worth knowing (not failures): the focus ring `outlineFocused` on a latched button is 1.12:1 against the fill, so focus on a latched button is invisible; `panelBackground` on `windowBackground` is 1.10 and the alternate row 1.05, as the designer says.

## 4. OK / warning / critical

Normal vision: unmistakable. L* 75.7 / 86.5 / 62.5, smallest pair difference dE2000 37.

Red-green deficiency, recomputed with two independent models (Machado 2009, and Vienot 1999 as a cross-check). They agree within about 1 L*, so the designer's "matrices from memory" caveat does not change anything.

| Viewer | L* green / amber / red | Green-amber | Amber-red | Green-red |
|---|---|---|---|---|
| Normal | 75.7 / 86.5 / 62.5 | dL 10.8, dE00 37 | dL 24.0, dE00 42 | dL 13.2, dE00 66 |
| Protan (Machado) | 78.8 / 83.8 / 54.3 | dL 5.0, dE00 19 | dL 29.5, dE00 26 | dL 24.5, dE00 21 |
| Deutan (Machado) | 73.6 / 88.1 / 67.1 | dL 14.5, dE00 26 | dL 21.0, dE00 16 | dL 6.5, dE00 20 |
| Protan (Vienot) | 79.5 / 84.5 / 53.7 | dL 5.0 | dL 30.8 | dL 25.9 |
| Deutan (Vienot) | 73.4 / 87.5 / 66.4 | dL 14.1 | dL 21.2 | dL 7.1 |

The designer's numbers are right and so is the self-assessment: separation by lightness is partial. Judgement: it is good enough in the meters (large areas, plus bar height) and in the event log (stripe plus the word), and a clear improvement on today (smallest simulated gap 2.1 L*, green-red dE00 8). It is **not** unmistakable for the 7 px status lamps seen alone from across a room: to a deuteranope "Running" is a neutral grey dot and "Device error" a khaki dot of nearly the same lightness. A palette cannot fix that, because the red is also 12 px text and has only 0.14 of headroom above 4.5:1 on a selected row. It needs a non-colour cue in code.

Fault versus fine or selected:

- **Latched pink versus critical red:** distinct for everyone (dE00 26 normal, 34 protan, 29 deutan).
- **Running lamp versus idle lamp under deuteranopia:** both are neutral greys (`#B7B5AC` and `#6F7380`), separated only by 25 L*. The teal green was chosen to turn grey for dichromats, which keeps it clear of amber and red, but it also puts it on the same axis as the idle lamp and the caption text (`textSecondary` simulates to `#A5A9B5`, 4 L* from the running lamp). Optional fix below.
- **Input traces 1 to 3 versus amber, for dichromats:** the yellow-green input base simulates to within dE00 4 to 8 of the warning amber. Traces are not status indicators, so I rate this cosmetic.
- **RMS cap:** at 18% the boundary between the RMS part and the peak part of a bar is only 3.6 L* on green and 2.3 L* on amber (13.9 and 11.4 today). The designer cut it to stop a red bar going salmon and approaching the accent, but the numbers do not support that: capped red sits dE00 22.0 from the accent at 18% and 21.6 at 40%. The cut costs RMS readability for no gain.

## 5. Per-channel hue rotation

`WaveformDisplay::colourForLane` (line 309): `base.withRotatedHue(0.045f * channelIndex)`, zero-based, base `traceReference` for outputs and `traceInputL` for inputs. Channels 1 to 8 with the proposed bases, on `windowBackground`:

| Ch | Output | Contrast | dE00 to red | Input | Contrast | dE00 to amber |
|---|---|---|---|---|---|---|
| 1 | `#809FFF` | 7.80 | 43.5 | `#B8EB75` | 14.23 | 22.1 |
| 2 | `#8380FF` | 6.09 | 42.4 | `#98EB75` | 13.57 | 26.8 |
| 3 | `#A680FF` | 6.71 | 40.1 | `#78EB75` | 13.07 | 30.1 |
| 4 | `#C880FF` | 7.49 | 38.0 | `#75EB92` | 13.18 | 31.6 |
| 5 | `#EA80FF` | 8.47 | 36.2 | `#75EBB1` | 13.38 | 33.4 |
| 6 | `#FF80F2` | 9.02 | 33.7 | `#75EBD1` | 13.65 | 36.0 |
| 7 | `#FF80CF` | 8.66 | 28.6 | `#75E5EB` | 13.28 | 40.2 |
| 8 | `#FF80AD` | 8.38 | 21.5 | `#75C5EB` | 10.26 | 48.8 |

- **Nothing vanishes.** Lowest contrast is 6.09 (output 2). Today inputs 7 and 8 are 2.29 and 2.52.
- **Nothing in 1 to 8 collides with warning or critical.** Closest is output 8 at dE00 21.5 from red. The two families stay apart (closest: output 1 and input 8, dE00 16.0).
- **Past 8 it does collide, as the designer says:** output 9 `#FF808B` and output 10 `#FF9880` are dE00 12.4 and 11.0 from the alarm red, and output 12 `#FFDC80` is dE00 5.2 from the warning amber. The shipping palette does the same (outputs 10 and 11). This matters on any rig with more than 8 outputs and can only be fixed in code.
- **Cost of the chosen input base:** inputs 1 to 4 are closer together than today, dE00 4.9, 3.7, 4.7 between neighbours against 7.3, 10.1, 13.7. In the mock, In 2 and In 3 are the same green to the eye. Each lane carries one channel and its own selector, so nothing is lost functionally, but on a 4-input interface the "tell inputs apart" purpose of the rotation is weaker than it is now.
- Outputs 6 to 8 are pinks within dE00 6 to 12 of the accent. Harmless, and it means the icon's pink does appear in the lanes on larger rigs.
- The designer's reason for a blue reference is sound: a pink base reaches dE00 7 from red by output 3.

## 6. Is the mock honest?

Substantially yes. Window 1120x720, 8 px margins, status bar 44, transport 56, event log 176, meter column 216: all match `MainComponent::resized`. Every colour in `theme_preview.svg` is a palette value or a tint the code actually computes. The lanes are drawn as two 1 px envelope lines, which is what the app draws (not a filled band). Fonts are JUCE's real macOS defaults (Lucida Grande, Menlo). Controls shown all exist: lane selectors, lane count, Follow, Clear, the count label, the Note column.

Where it departs:

- **It shows a MODE cell ("Pink noise loopback") that the current working tree no longer lays out** (`TopStatusBar.cpp`, uncommitted change). The designer's notes say the tree moved under them; this is the visible consequence.
- **It never shows a focus ring.** JUCE buttons take keyboard focus when clicked, and `drawButtonBackground` outlines the focused one in `outlineFocused`, so by the code the last-clicked control carries a 1 px ring in the new pink (not observed in the running app). So "pink is nearly absent in a normal run" is slightly understated in the app's favour and the mock's.
- It does not show the unmonitored "OFF" meter state (disclosed), and the scenario is contrived to put every colour on screen (disclosed).
- Something the palette cannot reach and the mock therefore hides: text selection and the caret in text editors still use JUCE's built-in cyan `#42A2C8`, because `DarkLookAndFeel` does not set those colour ids. That blended with the old steel-blue accent; beside a pink accent it will look like a leftover.

## 7. Things that could embarrass the owner

- **The plate.** Covered above; this is the one that would be noticed.
- **The ruler in the icon.** The left amber cursor lands on a tick (571.5); the right one (703.5) falls between ticks at 691 and 721, giving a doubled-tick look, and the 30.1 px pitch makes tick widths alternate between 5, 6 and 7 px (`ruler_crop.png`). On a measurement tool's icon, a marker that misses the graticule is the kind of thing an engineer sees. Visible at 256 px and up.
- **Composition sits low.** 159 px of space above the bracket, 113 px below the ruler; the band centre is 8 px below the body centre. Mild.
- **64 px.** The ruler turns to speckle (disclosed). A 64 px master without it would be cleaner.
- **Colours.** Not muddy. The meter "green" is a teal; defensible and explained, but worth telling the owner so it is not a surprise.
- **Resemblance.** I rendered Voice Memos beside it on this Mac; they are not alike. I did not and could not run a wider lookalike or trademark search.

## The designer's flagged weaknesses, one by one

| Flagged | Verified? | Does it matter? |
|---|---|---|
| Colour-vision lightness separation is partial | Yes, numbers reproduce on two models | Moderately. Fine for meters and the log, weak for lone status lamps. Not a palette problem. |
| Two text cases miss 4.5:1 | Yes (2.88, 3.76). There is a third, disabled-and-latched Mute at 1.24 | The hint is worth a one-line code fix. The pressed state does not matter. The third is a code fix. |
| Reference trace is blue, not pink | Yes, code confirms the rotation | Correct decision. |
| Pink nearly absent in a normal run | Yes: slider, Follow tick, focus ring only | Acceptable for test gear. The icon and UI are linked more by amber and graphite than by pink. |
| JUCE maths re-implemented from memory | Checked against JUCE source: off by at most 0.12 in a ratio, traces exact | No. |
| Colour-vision matrices from memory | Agree with an independent model within about 1 L* | No. |
| Superellipse approximates Apple's shape | Up to 3 px inside the template | **Yes. It triggers the plate at 512 and 1024.** |
| `ICON_BIG` would discard the hand-fitted small sizes | JUCE would write only 512 and 1024 | Yes, but the delivered `.icns` fares worse on this OS. The asset catalog is the route that keeps them. |

## Required changes

1. **Redraw the icon body to Apple's template shape** in the 64 px-and-up master: 824x824 at (100,100), continuous-corner radius 185.4, replacing the exponent-5 superellipse in `tools/gen_icons.py`. Reference mask: `exp/apple_mask_1024.png`.
2. **Do not ship `AppIcon.icns` as built.** Package as an asset catalog (`Assets.car`, `CFBundleIconName`) containing the corrected 64 to 1024 renders plus the existing hand-fitted 16 and 32 px files. Fallback if that is not wanted: point `ICON_BIG` at the corrected 1024 PNG with `ICON_SMALL` empty, and accept a system-scaled 16 px.
3. **In the same re-render, fix the ruler:** move the right edge of the gap from x=704 to x=722 (tick 17) so both cursors sit on ticks, and use a 30.0 px pitch starting at x=212.
4. **Check the result on the target Mac** in the Dock, a Finder list view and Get Info before calling it done.

No colour value has to change.

## Optional improvements

1. `meterRmsOverlay` `0x2effffff` to `0x4dffffff` (30%). RMS-to-peak step goes from 3.6 / 2.3 / 5.5 L* (green / amber / red) to 6.4 / 3.8 / 9.6; capped red stays dE00 21 from the accent.
2. `statusIdle` `0xff6e7480` to `0xff555a65`. Deutan lightness gap between the Running and Stopped lamps goes from 25 to 36 L*; still 2.59:1 on the panel.
3. If 4-input rigs matter more than 8-input ones: `traceInputL` `0xff75eb92` and `traceInputR` `0xff75ebb1`. Inputs 1 to 4 then differ by dE00 6.4, 8.6, 11.2. The price is that input 7 lands on output 2's blue (dE00 4.4) and input 8 close to output 3's. Owner's call.
4. Code, outside this deliverable: drop `.withAlpha (0.5f)` on the lane hint; give a disabled-and-latched button a readable label; set `TextEditor::highlightColourId` and `CaretComponent::caretColourId` from the palette; stop the hue rotation running into red and amber past channel 8; add a non-colour cue (shape or blink) to the critical lamp.
5. Icon polish: a 64 px master without the ruler; raise the composition about 20 px.
6. Regenerate the mock without the MODE cell and with one focused control before it is used to sell the theme.
7. Update the stale "steel-blue" comments the designer lists.

## What I could not verify

- **The real Dock and Finder.** Icon rendering was observed through `NSWorkspace.icon(forFile:)` on unsigned dummy bundles on this Mac only (macOS 27.0, dark icon appearance). I did not see it in the Dock, on the target Mac mini, on an older macOS, or in the light, tinted or clear icon appearances. The plate behaviour is undocumented system behaviour; the 512 px result in particular is borderline (plate for the delivered file, accepted for a resampled one).
- **The theme in the running app.** Nothing was built. All theme numbers are computed, and the mock is a drawing.
- **Whether Mute really stays latched after Stop Audio at runtime.** Read from the code only.
- **Colour-deficient viewers and the rack display.** Simulation only; the near-black surface steps (1.05 to 1.10:1) depend on the monitor's black level.
- **The Machado matrices against the paper.** They match my own recollection and an independent model, which is not the same as checking the source.
- **Lookalike icons.** No search was possible.
- **Later edits to the working tree.** Colour usage was read from the tree as it stood during this audit.

## Working files in this folder

`png.py`, `icon_check.py`, `bbox.py`, `sheet.py`, `jail.py`, `theme_check.py` (scripts); `icon_check.out`, `theme_check.out`, `extras.out` (outputs); `extracted.iconset/`; `probe/` and `exp/` (dummy bundles, variant icons, system renders); and the comparison images named in the text.
