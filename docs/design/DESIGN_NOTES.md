# Design notes: icon and theme for Pink Noise Audio Monitor

Nothing here has been integrated. The repo was read-only for this work; every file is in this folder. Nothing was built, launched or tested inside the app, the Dock or Finder. What I verified, I verified by rendering to PNG and looking at the PNG, or by computing it.

## Revision 3: pink to blue (owner feedback)

The owner liked icon A and the theme but did not want pink, and suggested "maybe like a razor blue". That is not a defined colour, so there are three candidates on `blue_options.png` and one recommendation taken to final.

| | Band colour | Reading |
|---|---|---|
| 1 | `#1FB0FF` | Electric: cold, high chroma, leaning toward cyan. The edge-of-a-blade reading. |
| 2 | `#8DB0CC` | Steel / ice: desaturated, metallic. |
| 3 | `#3391FF` | Honed: a truer, deeper blue. **Recommended, and what `final/` is built from.** |

Why 3. At 16 px it is the one that is plainly blue on both a dark and a light background: 1 goes sky-cyan and 2 goes pale grey-blue. In the UI the same value has to serve as the accent, and 3 sits further than 1 from the teal OK green and from the cyan end of the input traces. And it is the least neon of the saturated pair, which suits a screen in a dark room. The case for 1 is that it is the brightest on a dark Dock and closest to the "blade" idea; the case for 2 is that it is the most restrained, but it is close to the steel-blue accent the app ships with today and it is the weakest at 16 px.

What was done:

- **Icon.** Only the band, its noise fringe, the glow and the collapsed line changed colour. Body shape, ruler, bracket and geometry are exactly as fixed after the audit. The amber bracket is kept: amber against blue is a near-complementary pair and it reads better than it did against pink, at every size.
- **Small sizes.** The 16 and 32 px masters were redrawn per candidate and looked at enlarged (`extras/blue_options_zoom.png`), not hue-shifted. Two tones were changed on sight: steel's 16 px band is lighter and bluer than its large-size tone, because the large-size tone read as grey at 16 px, and candidate 3's 16 px band is lifted slightly so a 4 px band keeps its brightness on a dark background.
- **Final.** `final/` was rebuilt from candidate 3 and the system-render check was re-run in full. Result in `final/INTEGRATION.md` section 3 and in `verify/`: full-bleed, no plate, at all seven sizes; amber mark visible at 16 px.
- **Theme.** Nine `Palette` values changed; `theme.md` lists them at the top with the reason for each. The accent, its two variants, the focus ring and the on-accent text went from pink to blue. Two things then had to move out of blue because the code already used it: the reference/output traces (now a near-white) and the Info severity colour (now a cool grey). The input-trace base moved 8 degrees toward yellow so its cyan end stays clear of the accent. The OK / warning / critical colours and the two audit tweaks are untouched.
- **Checked.** Every contrast ratio and L* figure was recomputed. A new table in `theme.md` section 5 gives the distance from the accent to each thing it must not be mistaken for, for normal vision and two simulated dichromats; section 6 re-checks channels 1 to 8 after hue rotation.

What this revision costs or leaves weak:

- **Outputs are no longer distinguishable from each other by colour.** At 10% saturation the per-channel hue rotation only tints the white. That is deliberate (any hued base in the blue-to-violet range collapses onto the accent for dichromats, and a near-white can never rotate into red or amber), but the code's per-channel rotation now does nothing useful for outputs.
- **Info rows have no hue.** The stripe and the word are a light grey. If the owner wants Info to have a colour, violet is the candidate: a light violet such as `#B3A5F0` is 20 (CIEDE2000) from the accent family with normal vision but 8 for a simulated deuteranope and 4 for a protanope.
- **Closest remaining approach:** input lane 8 (cyan) against the lighter accent blue, 20.6 with normal vision and 15.5 for a simulated deuteranope. Past lane 8 the input family does rotate into the accent's blues; only code can stop that.
- **The name says pink and the icon is blue.** The band is still noise; it is just no longer a picture of the word.
- **`icon_contact_sheet.png` and `extras/zoom_small.png` are the first-round A/B/C comparison and still show pink.** They are kept as the record of the concept choice and are not rebuilt. The pink renders of A are in `icons/A_pink_superseded/`; `icons/A/` is now the blue final.
- If the owner picks candidate 1 or 2 instead, the icon is one constant (`FINAL_SCHEME` in `tools/gen_icons.py`) and a rebuild, but the theme's accent values and the input-trace base would need re-deriving; they were tuned for candidate 3.

## Revision 2: after the audit (2026-10-10)

The audit (`../design_audit/AUDIT.md`) confirmed concept A and passed the theme values, and found that the icon files as first delivered were drawn shrunk onto a black plate by macOS 27 at 16, 32, 512 and 1024 px. What changed in response:

- **Body shape.** The 64 px-and-up master of A now uses Apple's template shape exactly (824x824 at 100,100, continuous-corner radius 185.4), exported from SwiftUI by `tools/export_body_path.swift`. Against the auditor's reference mask, by my count the old body was 3,087 opaque pixels short; the new one is 51 short, all of them edge pixels where the two rasterisers antialias differently.
- **Ruler.** Ticks are on a 30.0 px pitch from x=212 and the gap's right edge moved from x=704 to x=722, so both amber cursors sit on ticks and all ticks are the same width.
- **Packaging.** The shipping icon is now an asset catalog, compiled with `actool`, in `final/`. The first delivery's `AppIcon.icns` and `AppIcon.iconset` have been deleted from this folder so they cannot be picked up by mistake.
- **Verification.** `tools/verify_icon.sh` puts the compiled catalog in a throwaway bundle and saves what macOS draws at 16, 32, 64, 128, 256, 512 and 1024 px. I looked at all seven: full-bleed, no plate, amber mark visible at 16 px. A control bundle with only the fallback `.icns` is plated at 16 px, which shows the check can tell the difference.
- **Palette.** `meterRmsOverlay` is now 30% (was 18%) and `statusIdle` is `#555A65` (was `#6E7480`), both on the auditor's recommendation. My stated reason for the 18% overlay was wrong; the audit's numbers show a capped red bar is no closer to the accent at 40% than at 18%.
- **Mock.** Redrawn without the MODE cell, which the working tree no longer lays out, and with the status-bar widths as they are in the tree now.
- **Maths.** The scripts' `brighter`, `darker` and `overlaidWith` now follow the JUCE 8.0.4 source; the contrast table moved by at most 0.12 and no pass or fail changed.

Not done, from the audit's optional list: a 64 px master without the ruler, raising the composition about 20 px, a focused control in the mock, and the alternative input-trace base for 4-input rigs. B and C were left as first delivered (superellipse body); they are comparison pieces only.

Sections below are the original notes from the pink delivery. They were corrected for the audit; for colour they are superseded by Revision 3 above wherever they say pink, a blue reference trace or a blue Info colour.

## Recommendation

**Icon: concept A, "Dropout", in blue `#3391FF`.** A band of noise runs edge to edge across a graphite body, collapses to a thin line for a short interval, and recovers. An amber bracket marks the start and end of the gap.

**Theme: "Graphite + Blue"**, the 32 values in `theme.md`. The accent is the icon's blue; green, amber and red are kept for level and severity only.

## Files

| File | What it is |
|---|---|
| `icon_1024.png` | Recommended icon, 1024x1024, transparent outside the body. |
| `final/` | **What ships.** `Assets.car`, the fallback `AppIcon.icns`, the partial Info.plist, the source `Assets.xcassets/`, `icon_1024.png`, and `INTEGRATION.md`. |
| `verify/` | The throwaway bundles, what macOS drew for them at seven sizes (`system_renders/`), the icns-only control, and two inspection sheets. |
| `blue_options.png` | Icon A in the three blue candidates at 256, 64, 32, 16 px on dark and light, labelled 1/2/3 with the band hex. |
| `icon_contact_sheet.png` | First round, still pink: A, B, C at 256, 64, 32, 16 px on dark and light backgrounds. Kept as the record of the concept choice. |
| `icons/blue1`, `icons/blue2`, `icons/blue3` | The three blue candidates of A: masters and PNGs at all seven sizes. |
| `icons/A_pink_superseded` | The pink A as audited (template body, corrected ruler). Not for use. |
| `icons/A` (final, blue), `icons/B`, `icons/C` (first round, pink) | Per concept: `concept_X.svg` (1024 master, used for 64 px and up), `concept_X_small32.svg`, `concept_X_small16.svg` (pixel-fitted masters), and PNGs at 16, 32, 64, 128, 256, 512, 1024. |
| `theme.md` | Palette table, paste-ready values, contrast tables, colour-vision tables, trace table, limitations. |
| `theme_preview.png` / `.svg` | 1120x720 mock of the main window in the proposed palette. |
| `theme_states.png` | Not asked for. Swatches (old beside new), button states, lamps, the colour-vision simulation, trace families. |
| `extras/blue_options_zoom.png` | 16 and 32 px renders of the three blue candidates, enlarged. |
| `extras/zoom_small.png` | First round, still pink: 16 and 32 px renders of all three concepts enlarged with nearest-neighbour, on three backgrounds. This is what I judged small-size legibility from. |
| `extras/theme_preview_shipping_palette.png` | The same mock drawn with the palette that ships today, for a side-by-side. |
| `tools/` | The scripts that generated everything. `tools/build_all.sh` rebuilds the lot. Needs Python (stdlib only), headless Chrome, `swift` and Xcode's `actool`. |

## Icon

### The three concepts

**A. Dropout (recommended).** It shows the thing the app exists to catch, in the form the app's own waveform lanes show it: noise, then nothing, then noise. The bracket is the "one start, one end per fault" rule from the logging invariants. Pink is the signal; amber is the event. Both colours are palette values (`accent`, `meterAmber`), so the icon and the UI share them exactly.

**B. Flat octaves.** Pink noise has equal energy per octave, so on an octave analyser it is flat. Four equal pink bars with one fallen to a white stub and its expected height ghosted in a dashed outline.

**C. Slope.** The other way to say "pink noise": a straight line falling at 3 dB per octave across a log-frequency grid. Done as the inverse scheme, dark line on a pink body, so the owner sees what a pink-bodied icon would look like.

### Why A

- At 16 and 32 px it keeps two things: a pink stripe with a break, and an amber mark over the break. B keeps its idea too but reads as a bar chart. C becomes a slash on a pink square, which reads as "disabled".
- It is the only one that says "fault", not just "audio" or "spectrum".
- B is the most legible glyph of the three at 16 px, and I think it is the most generic. If the owner prefers plain legibility over meaning, B is the fallback.

### How the small sizes were handled

The 16 and 32 px icons are not the big artwork scaled down. Each concept has its own master for those sizes with every edge on the target pixel grid, gradients simplified, and detail removed (no noise texture, no ruler; at 16 px the thin "collapsed" line is dropped and the gap is left empty). I looked at these enlarged (`extras/zoom_small.png`) and at true size (contact sheet).

The first version of A had the band inset from the body edges. At 16 and 32 px it read as a dumbbell. Running the band edge to edge fixed that and is why the artwork touches the sides of the body.

### Weaknesses of the icon

- **Genre overlap.** A waveform on a dark rounded square is a crowded genre; Voice Memos is the obvious neighbour, and many audio editors use something similar. A differs in what it draws (a solid noise band with a rectangular gap and a bracket, not a row of bars or a speech waveform) and in colour, but I did no trademark or store search. "Does not resemble an existing icon" rests on my memory of well-known icons, nothing more.
- **16 px is a stripe with a gap.** The dropout idea survives only as that gap and a 2 px amber dash. Someone who has never seen the large icon would not decode it.
- **It idealises the real display.** `WaveformDisplay` strokes two 1 px envelope lines per lane; it does not fill a band. The icon's filled band is what noise looks like in most editors, not exactly what this app draws.
- **Body size at 16 and 32 px is snapped**, to 14 px and 26 px, instead of the proportional 12.9 and 25.8 px. Sharper edges, slightly less margin than the template. Rendering those sizes from the 1024 master instead is a one-line change in `tools/gen_icons.py`.
- **64 px uses the full master.** The ruler ticks at the bottom are below a pixel wide there and render as faint texture. Harmless, but a 64 px master without them would be cleaner.
- **Shape (fixed for A).** The first delivery used a superellipse that sat up to 3 px inside Apple's template, which was enough for macOS 27 to reject it. A's 64 px-and-up master now uses the template path. The 16 and 32 px masters, and all of B and C, still use the superellipse; the system accepts A's small ones when they come from an asset catalog.
- **Baked-in shadow, no layered source.** The 64 px and larger renders include the template-style drop shadow inside the canvas. As drawn by `NSWorkspace.icon(forFile:)` on this Mac, the system re-masks the icon and adds its own edge highlight, and the edge-to-edge band survives that. I have still not seen it in the Dock itself. No Icon Composer `.icon` file was made.
- **Lower edge on near-black.** The bottom of the body is 1.12:1 against a `#101114` background. The rim highlight keeps the top and sides visible; the bottom edge is faint on a very dark Dock.
- B at 16 px sits half a pixel right of centre (four 2 px bars with 1 px gaps cannot centre on an even grid).

### Rejected before rendering

- A level trace with a square notch: reads as a square wave, and at 16 px its silhouette is the same as B's.
- A ring or loop for "send and return": reads as refresh, progress or power.
- A VU needle: the stock metering picture, and the needle disappears at 16 px.
- Two tall bars, send beside return: a pause symbol.
- Any lettering: the brief asked for none.

## Theme

### What I optimised for

1. Text that passes 4.5:1 wherever the code draws it, including the tinted critical rows and the selected row.
2. One green, one amber, one red, identical in meters, lamps and the event log, with a fixed lightness order.
3. Pink only where the app uses an accent: latched buttons, the slider, toggle ticks, focus rings.
4. Values only. No entry added, none removed, no code assumed to change.

### Decisions the auditor may want to challenge

- **The reference trace is blue, not pink.** `colourForLane` rotates hue by +16.2 degrees per channel. A pink base puts output 3 on red. With the bases chosen, eight outputs run blue to pink and eight inputs run yellow-green to cyan without overlapping or touching red or amber. Past channel 8 both families eventually do; no base value prevents that. `theme.md` section 5 has the table and the one-line code change that would allow a pink reference.
- **Input 1 is yellow-green rather than the current mid green.** That is the price of the point above. It is the change most likely to be disliked on sight.
- **`severityInfo` is blue, not the accent.** Today info, accent and reference trace are one colour. Pink info stripes beside red critical stripes, 3 px wide, seen from across a room, seemed a bad trade.
- **The accent is nearly absent in a normal run.** With audio running and logging on, pink appears on the slider and the Follow tick and nowhere else in the main window. It becomes large only when Mute or Pause is latched, which is when attention is wanted. "Reads as one system" therefore rests mostly on those states plus the shared amber. `theme_preview.png` shows the normal state on purpose; `theme_states.png` shows the latched one.
- **Brighter than the current theme.** Text, meters and state colours are all lighter. That is for distance legibility; it is somewhat less "low glare" than what ships.
- **The mock's scenario is contrived.** A dropout, a clip and a level change at once, 38 hours in, so that every state colour is on screen.

### Where it falls short of the brief

- **Colour-vision separation is partial.** The brief asks that OK, warning and critical differ in lightness. With normal vision every pair is at least 10.8 L* apart, in the same order for every viewer (red darkest, amber lightest). Under simulated protanopia the green-to-amber gap shrinks to 5.0 L*, and under simulated deuteranopia green-to-red is 6.5 L*. I could not widen these: red is also 12 px text and needs 4.5:1 on a tinted row, which stops it going darker; amber cannot go much lighter. Chroma does the rest (smallest simulated delta E between any pair is 29), and every lamp and stripe has a word beside it. The shipping palette's smallest simulated gap is 2.1 L*. A deeper fill-only red for `meterRed` (`#E8432E`) would widen the deutan green-red gap on the meters to about 15 L* at the cost of having two reds; I chose one red.
- **Two text cases fail 4.5:1** and cannot be fixed by a value: the half-alpha "(no channel selected)" hint (2.85:1), and a latched button's label during the moment the mouse is held down on it (3.78:1). Both are explained in `theme.md` section 2.
- **Small captions stay small.** The 9 to 9.5 unit captions now have 7.7:1 contrast, but they are about 8 px text. Legibility across a room is limited by their size, which a palette does not change.
- **The unfilled slider track is faint** (1.18:1), as it is today.

### Rejected

- Pink as the info colour and as the reference trace (above).
- A darker latched-button fill with light text: `textOnAccent` would then fail on `accent` itself if any later code pairs them.
- Ordering the alarm colours red, amber, green by lightness. It gives larger simulated gaps, but needs a pale mint "OK" and an orange "warning"; the meters would glare and amber would sit closer to red.
- Two greens or two ambers (a lighter one for lamps). Each fixes one simulated pair and breaks another.
- (Withdrawn.) I first cut the RMS overlay to 18% on the grounds that 40% turned a red bar salmon, too close to the accent. The audit measured it and the grounds were wrong; the overlay is now 30%.

## What I am unsure about

- **The mock is a drawing.** Layout numbers come from the source, but fonts are my assumption (Lucida Grande and Menlo as JUCE's macOS defaults, em size taken as 0.85 of JUCE's font height). Real text may be a little larger or smaller. It also does not show the unmonitored-input "OFF" meter state described below.
- **JUCE colour maths was re-implemented, not run.** `brighter`, `darker` and `withRotatedHue` in `tools/` follow my reading of JUCE. If any is off, the hover and pressed rows and the trace table shift.
- **The colour-vision matrices were entered from memory** (Machado 2009, full severity). Each row sums to one, which is the only check I made. No colour-deficient person looked at this.
- **Display dependence.** Window to panel is 1.10:1 and panel to alternate row is 1.05:1. On a display with raised blacks they may merge.
- **Icon integration route.** Now written up in `final/INTEGRATION.md`. The catalog is verified in a dummy bundle; the CMake lines that would put it into the real app are from reading JUCE's source and have not been run.
- **Stale text after integration.** The header's "steel-blue" comments and the hex table in `UI_DESIGN.md` section 2 will be wrong once the values change. `theme.md` section 1 lists them.

## The repo changed while I worked

The repo was clean when I started. Partway through, `git status` showed twelve modified source files, and seventeen by the time I finished, none of them mine and none of them the palette header (`DarkLookAndFeel.h` and `.cpp` are unchanged). One change (unmonitored inputs drawn at 25% alpha with an "OFF" label) uses palette colours in a new way; I checked it against the new values in `theme.md` section 7. I read the UI sources before those edits appeared and re-checked only the meter change, so anything else they altered about where colours are used, or about layout, is not reflected in the theme tables or the mock.

I ran plain `git status` once before switching to `--no-optional-locks`. That command can rewrite the stat cache in `.git/index`. No tracked or untracked file in the repo was created, edited or deleted by me.
