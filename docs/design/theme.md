# Theme: Graphite + Blue

One dark theme for Pink Noise Audio Monitor. It maps one-to-one onto the existing `pnam::Palette` constants in `Source/UI/DarkLookAndFeel.h`: every entry is kept, none is added, and applying it means changing the 32 values and nothing else. The two dB zone thresholds (`meterAmberFromDb`, `meterRedFromDb`) are not colours and are unchanged.

Every contrast ratio, L*, delta E and hue figure in this file is computed by `tools/gen_theme_docs.py` from `tools/palette.py` rather than typed in. Contrast is the WCAG 2.x ratio. Semi-transparent colours are composited first, in sRGB. JUCE's `brighter`, `darker` and `overlaidWith` follow the JUCE 8.0.4 source in the repo's build tree (truncating integer maths); `withRotatedHue` is a re-implementation that the audit found matches JUCE hex for hex.

## Revision history

1. First delivery: pink accent, matching the first icon.
2. After the audit (`../design_audit/AUDIT.md`): `meterRmsOverlay` 18% to 30% alpha and `statusIdle` `#6E7480` to `#555A65`, both on the auditor's recommendation. Both are kept.
3. This revision: the owner asked for blue instead of pink. The accent is now the icon's blue, and because the app already used blue for the reference traces and for Info rows, those two were moved out of blue. 9 entries changed from revision 2; the other 23 did not.

| Entry | Revision 2 (pink) | Now | What moved |
|---|---|---|---|
| `outlineFocused` | `#D0689F` | `#55A4FF` | pink to blue (accent) |
| `textOnAccent` | `#14080E` | `#050D1A` | plum-black to navy-black |
| `accent` | `#EC70AE` | `#3391FF` | pink to blue |
| `accentDim` | `#C85F95` | `#3585E6` | pink to blue |
| `accentBright` | `#FFA6CF` | `#55A4FF` | pink to blue |
| `traceReference` | `#809FFF` | `#CFD4E6` | blue to near-white (collision with the accent) |
| `traceInputL` | `#B8EB75` | `#C7EB75` | yellow-green, 8 degrees yellower (room for the accent at the cyan end) |
| `traceInputR` | `#98EB75` | `#A8EB75` | follows traceInputL (unused entry) |
| `severityInfo` | `#8FB4E3` | `#BCC2CD` | blue to cool grey (collision with the accent) |

Unchanged from revision 2: `windowBackground`, `panelBackground`, `panelBackgroundAlt`, `controlBackground`, `controlHover`, `controlPressed`, `outline`, `textPrimary`, `textSecondary`, `textDisabled`, `meterGreen`, `meterAmber`, `meterRed`, `meterTrough`, `meterPeakTick`, `meterRmsOverlay`, `traceGrid`, `traceCentreLine`, `severityWarning`, `severityCritical`, `statusRunning`, `statusIdle`, `statusPaused`.

## 1. Palette

| Entry | Current | Proposed | Why |
|---|---|---|---|
| `windowBackground` | `#0E1114` | `#0A0B0D` | Deeper, near-neutral graphite so panels separate from the window (1.10:1 instead of 1.07:1) and the waveform lanes, which sit directly on it, get a blacker field. |
| `panelBackground` | `#16191D` | `#15171B` | Neutral graphite with a trace of blue; a touch lighter than the window, dark enough that critical-row red text still clears 4.5:1. |
| `panelBackgroundAlt` | `#1A1E23` | `#1A1C21` | Alternate table rows and table header. Kept close to the panel (1.05:1) so zebra striping never competes with the severity tint. |
| `controlBackground` | `#1C2126` | `#23262D` | Buttons, combos, editors. Lifted further from the panel (1.18:1, was 1.09:1) so controls read as controls from a distance. |
| `controlHover` | `#242A31` | `#2B2E37` | Hover, selected table row, scrollbar thumb. Capped at this lightness because critical red text must stay at 4.5:1 on a selected row. |
| `controlPressed` | `#2B323A` | `#363A45` | One clear step above hover; also the pressed table-header and scrollbar-thumb fill. |
| `outline` | `#262C33` | `#353942` | Hairlines were 1.25:1 on the panel and vanished across a room; now 1.55:1, still quieter than any text. |
| `outlineFocused` | `#3C586E` | `#55A4FF` | Keyboard-focus ring, the same lighter blue as accentBright so it shows on a dark control: 5.9:1 on a button, 7.0:1 on the panel (ships at 2.4:1, below the 3:1 a focus indicator needs). Kept saturated, not pale, so it cannot be taken for a reference trace. |
| `textPrimary` | `#D7DBDF` | `#E8EAEE` | Slightly brighter and cooler white for readouts and table cells; 14.9:1 on the panel. |
| `textSecondary` | `#8B939B` | `#A4AAB5` | Captions, ruler numbers, table headers. Raised so the small captions hold 7.7:1 and stay at 4.9:1 on a pressed header. |
| `textDisabled` | `#525A62` | `#6E7480` | Disabled labels and the idle lamp. Lifted from 2.3:1 to 3.2:1 on a button so a disabled control is still readable as a word. |
| `textOnAccent` | `#0E1114` | `#050D1A` | Text on a latched button. Near-black with a hint of navy; 5.2:1 on accentDim (the shipping pair is 2.7:1). |
| `accent` | `#5B93B8` | `#3391FF` | The icon's blue, the same value as the middle of its band. Slider fill and toggle ticks; 5.7:1 on the panel. To red-green dichromats it stays a strong blue. |
| `accentDim` | `#3A5D75` | `#3585E6` | Fill of a latched button (Mute, Pause). One step deeper than the accent and still light enough to carry dark text at 5.2:1. |
| `accentBright` | `#7FB3D3` | `#55A4FF` | Slider thumb. A lighter blue than the accent but deliberately still saturated: a pale ice blue would sit too close to the reference traces and the Info rows. |
| `meterGreen` | `#3FAE6A` | `#3BD1A9` | Unchanged by the move to a blue accent. Blue-leaning green, L* 76; reads as neutral grey to red-green dichromats, which keeps it apart from the two yellowish alarm colours and from the accent, which stays blue for them (delta E2000 42 from the accent with normal vision, 36 under simulated deuteranopia). |
| `meterAmber` | `#D9A53C` | `#FFD452` | Golden amber, L* 86, the lightest of the three so it never falls between green and red in lightness. |
| `meterRed` | `#D14B4B` | `#FF6450` | Vermilion, L* 62, the darkest of the three. |
| `meterTrough` | `#14171B` | `#0E0F12` | Unlit bar well, a step darker than the panel so the lit bar has the most room. |
| `meterPeakTick` | `#E8ECEF` | `#F6F7F9` | Peak-hold line, near-white, 17.9:1 on the trough. |
| `meterRmsOverlay` | `#FFFFFF @ 40%` | `#FFFFFF @ 30%` | Translucent white RMS cap, 30% (ships at 40%). Light enough that the step between the RMS part and the peak part of a bar stays readable (6.4 L* on green, 3.8 on amber, 9.6 on red). My first proposal was 18%; the audit showed that made the step too small and that the reason I gave for it (keeping a capped red bar away from the accent) did not hold. |
| `traceReference` | `#5B93B8` | `#CFD4E6` | MOVED from cornflower blue to a near-white with a faint cool tint. A blue accent and a blue reference family cannot coexist: the nearest output lane was within delta E2000 8 of the accent with normal vision and 1 for a deuteranope. At 10% saturation the per-channel hue rotation only tints the white, so every output lane, all 64, stays at least 18 from the accent for every viewer and can never rotate into red or amber. Reference in white, measured input in colour, is the usual instrument convention. |
| `traceInputL` | `#3FAE6A` | `#C7EB75` | Yellow-green, the base of the input family; moved 8 degrees toward yellow (hue 78) so that input 8, where the rotation ends, stops at cyan and stays clear of the blue accent. |
| `traceInputR` | `#B87FD3` | `#A8EB75` | Not referenced by any drawing code today. Set to the colour the input family gives its second channel, so it is consistent if it is used later. |
| `traceGrid` | `#20262C` | `#1C1E24` | Not referenced by any drawing code today. A grid line a step quieter than the centre line, for when a grid is added. |
| `traceCentreLine` | `#2C343C` | `#2D3039` | Zero line in each lane; 1.5:1 on the window, visible but well under the traces. |
| `severityInfo` | `#5B93B8` | `#BCC2CD` | MOVED from blue to a cool light grey. With a blue accent, a blue Info colour was within delta E2000 8 of it. Info rows now carry no hue at all: colour in the event log means warning or critical, and the Follow tick and any focus ring beside the table are the only blue there. 10.0:1 on the panel. |
| `severityWarning` | `#D9A53C` | `#FFD452` | Same value as meterAmber: one amber everywhere. |
| `severityCritical` | `#D14B4B` | `#FF6450` | Same value as meterRed: one red everywhere. 5.1:1 on a tinted critical row, 4.6:1 on a selected row. |
| `statusRunning` | `#3FAE6A` | `#3BD1A9` | Same value as meterGreen. |
| `statusIdle` | `#525A62` | `#555A65` | A darker neutral than textDisabled, so a stopped lamp sits further in lightness from the running lamp, which red-green dichromats also see as a grey (36 L* apart under simulated deuteranopia). 2.59:1 on the panel: visibly an unlit lamp, deliberately below 3:1. Changed from my first proposal on the audit's recommendation. |
| `statusPaused` | `#D9A53C` | `#FFD452` | Same value as meterAmber. |

Values as they go into the header (AARRGGBB):

```cpp
static constexpr juce::uint32 windowBackground   = 0xff0a0b0d;
static constexpr juce::uint32 panelBackground    = 0xff15171b;
static constexpr juce::uint32 panelBackgroundAlt = 0xff1a1c21;
static constexpr juce::uint32 controlBackground  = 0xff23262d;
static constexpr juce::uint32 controlHover       = 0xff2b2e37;
static constexpr juce::uint32 controlPressed     = 0xff363a45;
static constexpr juce::uint32 outline            = 0xff353942;
static constexpr juce::uint32 outlineFocused     = 0xff55a4ff;
static constexpr juce::uint32 textPrimary        = 0xffe8eaee;
static constexpr juce::uint32 textSecondary      = 0xffa4aab5;
static constexpr juce::uint32 textDisabled       = 0xff6e7480;
static constexpr juce::uint32 textOnAccent       = 0xff050d1a;
static constexpr juce::uint32 accent             = 0xff3391ff;
static constexpr juce::uint32 accentDim          = 0xff3585e6;
static constexpr juce::uint32 accentBright       = 0xff55a4ff;
static constexpr juce::uint32 meterGreen         = 0xff3bd1a9;
static constexpr juce::uint32 meterAmber         = 0xffffd452;
static constexpr juce::uint32 meterRed           = 0xffff6450;
static constexpr juce::uint32 meterTrough        = 0xff0e0f12;
static constexpr juce::uint32 meterPeakTick      = 0xfff6f7f9;
static constexpr juce::uint32 meterRmsOverlay    = 0x4dffffff;
static constexpr juce::uint32 traceReference     = 0xffcfd4e6;
static constexpr juce::uint32 traceInputL        = 0xffc7eb75;
static constexpr juce::uint32 traceInputR        = 0xffa8eb75;
static constexpr juce::uint32 traceGrid          = 0xff1c1e24;
static constexpr juce::uint32 traceCentreLine    = 0xff2d3039;
static constexpr juce::uint32 severityInfo       = 0xffbcc2cd;
static constexpr juce::uint32 severityWarning    = 0xffffd452;
static constexpr juce::uint32 severityCritical   = 0xffff6450;
static constexpr juce::uint32 statusRunning      = 0xff3bd1a9;
static constexpr juce::uint32 statusIdle         = 0xff555a65;
static constexpr juce::uint32 statusPaused       = 0xffffd452;
```

Comments in the header that the new values make stale, for whoever integrates this: the file banner ("desaturated steel-blue accent"), the `// -- Accent (desaturated steel blue)` heading, and the trailing `// accent`, `// green`, `// muted violet` notes on the three trace entries. `UI_DESIGN.md` section 2 repeats the old hex values in a table.

## 2. Text contrast

Requirement: 4.5:1 for body text, 3:1 for large status text. Nothing in this UI is large by the WCAG definition (the biggest text is the 16-unit button label), so every row is held to 4.5:1.

| Text colour | Drawn on | Where | Ratio | 4.5:1 | Current palette |
|---|---|---|---|---|---|
| `textPrimary` | panelBackground | status values, meter readout, table cells, labels, dialogs | **14.90** | pass | 12.67 |
| `textPrimary` | panelBackgroundAlt | table cells on odd rows | **14.15** | pass | 12.03 |
| `textPrimary` | critical row, even (panel + 10% red) | table cells on a critical row | **13.12** | pass | 11.59 |
| `textPrimary` | critical row, odd (alt + 10% red) | table cells on a critical row | **12.43** | pass | 11.02 |
| `textPrimary` | controlBackground | buttons, combo boxes, text editors, slider value box | **12.57** | pass | 11.65 |
| `textPrimary` | button hover (controlBackground.brighter 0.08) | hovered button | **10.06** | pass | 9.43 |
| `textPrimary` | button down (controlBackground.darker 0.2) | pressed button | **13.67** | pass | 12.44 |
| `textPrimary` | controlHover | selected table row, highlighted menu item, hovered status cell | **11.26** | pass | 10.40 |
| `textSecondary` | panelBackground | status captions (9.5), meter title (13), ruler (9), channel names (9.5) | **7.69** | pass | 5.66 |
| `textSecondary` | panelBackgroundAlt | table header (13) | **7.30** | pass | 5.38 |
| `textSecondary` | controlHover | hovered table header | **5.81** | pass | 4.65 |
| `textSecondary` | controlPressed | pressed table header | **4.87** | pass | 4.16 |
| `textDisabled` | controlBackground | disabled button label (WCAG exempts disabled controls) | **3.23** | exempt | 2.31 |
| `textDisabled` | panelBackground | disabled toggle label (exempt) | **3.82** | exempt | 2.52 |
| `textDisabled` | accentDim | a button that is disabled while still latched (Mute after Stop Audio); exempt, but see below | **1.26** | exempt | 1.00 |
| `textOnAccent` | accentDim | latched button label (Mute, Resume) | **5.24** | pass | 2.71 |
| `textOnAccent` | accentDim hover (brighter 0.08) | latched button, hovered | **5.80** | pass | 3.25 |
| `textOnAccent` | accentDim down (darker 0.2) | latched button while the mouse is held down | **3.79** | FAIL | 2.13 |
| `severityInfo` | panelBackground | 'Info' in the Severity column (12) | **10.03** | pass | 5.30 |
| `severityInfo` | panelBackgroundAlt | same, odd row | **9.53** | pass | 5.03 |
| `severityInfo` | controlHover | same, selected row | **7.58** | pass | 4.35 |
| `severityWarning` | panelBackground | 'Warning' in the Severity column (12) | **12.64** | pass | 7.89 |
| `severityWarning` | panelBackgroundAlt | same, odd row | **12.01** | pass | 7.49 |
| `severityWarning` | controlHover | same, selected row | **9.55** | pass | 6.48 |
| `severityCritical` | critical row, even (panel + 10% red) | 'Critical' in the Severity column (12) | **5.41** | pass | 3.70 |
| `severityCritical` | critical row, odd (alt + 10% red) | same, odd row | **5.13** | pass | 3.52 |
| `severityCritical` | controlHover | same, selected row | **4.64** | pass | 3.32 |
| `severityWarning` at 85% alpha | windowBackground | 'FROZEN' tag on the waveform (12) | **10.09** | pass | 6.40 |
| `textSecondary` at 50% alpha | windowBackground | '(no channel selected)' placeholder (11) | **2.85** | FAIL | 2.39 |

Two rows fail and neither can be fixed by a palette value alone, and one exempt row is unreadable in practice:

- **`textSecondary` at 50% alpha** (`WaveformDisplay.cpp`, the "(no channel selected)" hint). The code halves the alpha, so `textSecondary` would have to be as light as `textPrimary` to reach 4.5:1, which would flatten the text hierarchy everywhere else. It is an empty-state hint, not data. Dropping the `.withAlpha (0.5f)` would give 8.43:1.
- **`textOnAccent` on a latched button while the mouse button is held down.** `drawButtonBackground` darkens the fill by `darker (0.2f)` for that moment. Passing it would need `accentDim` as light as `accent` itself, which would make a latched Mute button glare. The resting and hovered states pass.

- **`textDisabled` on `accentDim` (1.26:1).** Found by the audit, not by me. From the code, Mute can stay latched when audio stops, and the button is then both disabled and toggled, so its label is drawn in `textDisabled` on the blue fill. Disabled controls are exempt from WCAG, but the word cannot be read. It is 1.00:1 today. It needs a code change (a different label colour for a disabled, latched button), not a value.

For comparison the shipping palette fails 9 of these rows, including the latched-button label (2.71:1) and `Critical` on a critical row (3.52:1).

## 3. Non-text contrast

| Element | Against | Ratio | Note |
|---|---|---|---|
| statusRunning lamp | panelBackground | 9.30 | 7 px lamp |
| statusPaused lamp | panelBackground | 12.64 |  |
| severityCritical lamp | panelBackground | 6.14 | Device error / Write error |
| statusIdle lamp | panelBackground | 2.59 | an unlit lamp; deliberately below 3:1 (see the palette table) |
| meterGreen fill | meterTrough | 9.93 |  |
| meterAmber fill | meterTrough | 13.50 |  |
| meterRed fill / clip lamp | meterTrough | 6.56 |  |
| meterPeakTick | meterTrough | 17.88 | drawn above the lit bar |
| accent (slider fill, toggle tick) | controlBackground | 4.79 |  |
| accentBright (slider thumb) | panelBackground | 6.96 |  |
| outlineFocused (focus ring) | controlBackground | 5.87 | 3:1 needed |
| outline (hairlines) | panelBackground | 1.55 | decorative; deliberately quiet |
| controlBackground (button body) | panelBackground | 1.18 | buttons are identified by their label, not this edge |
| controlBackground (slider track) | panelBackground | 1.18 | the unfilled track is faint; see limitations |
| traceCentreLine | windowBackground | 1.49 | decorative |

## 4. OK / warning / critical under red-green colour-vision deficiency

Simulated with the Machado, Oliveira and Fernandes (2009) matrices at full severity (protanopia, deuteranopia), applied in linear RGB. The matrices are in `tools/palette.py`; I entered them from memory and checked only that each row sums to one, so treat them as worth a second look. A simulation is also not a test with colour-deficient viewers. L* is CIE lightness; delta E is CIE76. One green, one amber and one red are used everywhere (meter zones, status lamps, severity stripes and words), so this one table covers all three uses.

| Viewer | Green (OK) | Amber (warning) | Red (critical) | L* green / amber / red | Green-amber: dL*, dE | Amber-red: dL*, dE | Green-red: dL*, dE |
|---|---|---|---|---|---|---|---|
| normal vision | `#3BD1A9` | `#FFD452` | `#FF6450` | 76 / 86 / 62 | 10.8, 78 | 24.0, 66 | 13.2, 112 |
| protan | `#CBC3A7` | `#EBD141` | `#8E824D` | 79 / 84 / 54 | 5.0, 56 | 29.5, 50 | 24.5, 29 |
| deutan | `#B7B5AC` | `#F5DE58` | `#B4A44C` | 74 / 88 / 67 | 14.5, 64 | 21.0, 29 | 6.5, 43 |

The lightness order is the same for all three viewers: red darkest, green in the middle, amber lightest. With normal vision every pair is at least 10.8 L* apart. Under simulation the smallest gap is **5.0 L*** (the shipping palette's smallest is 2.1 L*). That is a real lightness difference but not a large one, and I could not make it larger: red is also 12 px text and must keep 4.5:1 on a tinted row, which sets a floor on how dark it can go, and amber cannot go much lighter and still be amber. What carries the rest is chroma: to a red-green dichromat the green turns neutral grey while amber stays a vivid yellow and red becomes a dark olive, so the smallest simulated colour difference between any pair is delta E 29.

Colour is never the only signal in the UI as written: each status lamp sits beside a word (Running, Paused, Device error), each severity stripe has the word in the Severity column, and the meter also shows level by height.

The severity trio in the event log (info / warning / critical):

| Viewer | Info | Warning | Critical | L* | Info-warning dE | Info-critical dE | Warning-critical dL* |
|---|---|---|---|---|---|---|---|
| normal vision | `#BCC2CD` | `#FFD452` | `#FF6450` | 78 / 86 / 62 | 74 | 77 | 24.0 |
| protan | `#BFC2CE` | `#EBD141` | `#8E824D` | 79 / 84 / 54 | 77 | 44 | 29.5 |
| deutan | `#BDC1CD` | `#F5DE58` | `#B4A44C` | 78 / 88 / 67 | 74 | 55 | 21.0 |

Info is now a neutral grey, so it is separated from warning and critical by having no hue at all. To a red-green dichromat the OK green is also a grey (`#B7B5AC`, against Info's `#BDC1CD`), but the two never share a role: Info is a stripe and a word in the event log, green is a lamp and a meter fill.

## 5. Is the blue accent confusable with anything else?

The accent family is `accent` (`#3391FF`), `accentDim` (`#3585E6`), and `accentBright` and `outlineFocused` (both `#55A4FF`). The table gives, for each thing it must not be mistaken for, the smallest CIEDE2000 distance to any member of that family, with normal vision and as simulated for protanopia and deuteranopia. For scale: the audit treated distances around 20 as clearly apart and distances near 10 or below as collisions.

| Against | Colour | Normal | Protan | Deutan | Closest accent entry (normal) |
|---|---|---|---|---|---|
| Output lanes 1 to 8, closest (lane 1) | `#CFD4E6` | 23.8 | 18.3 | 22.7 | `accentBright` |
| Output lanes 1 to 64, closest (lane 22) | `#CFDCE6` | 22.3 | 18.3 | 22.2 | `accentBright` |
| Input lanes 1 to 8, closest (lane 8) | `#75D4EB` | 20.6 | 16.2 | 15.5 | `accentBright` |
| Info rows (`severityInfo`) | `#BCC2CD` | 21.2 | 20.1 | 22.5 | `accentBright` |
| OK green (`meterGreen`) | `#3BD1A9` | 38.7 | 37.8 | 31.8 | `accentBright` |
| Warning amber (`meterAmber`) | `#FFD452` | 56.9 | 57.5 | 61.6 | `accentBright` |
| Critical red (`meterRed`) | `#FF6450` | 46.8 | 47.2 | 54.3 | `accentDim` |
| Idle lamp (`statusIdle`) | `#555A65` | 25.6 | 27.7 | 24.6 | `accentDim` |
| Body text (`textPrimary`) | `#E8EAEE` | 28.8 | 26.8 | 29.9 | `accentBright` |
| Caption text (`textSecondary`) | `#A4AAB5` | 19.3 | 18.9 | 20.1 | `accentBright` |

The smallest figure anywhere in the table is 15.5. The closest thing to the accent with normal vision is input lane 8 (20.6), the cyan end of the input family against the lighter accent blue; inputs 1 to 6 are all above 40. Against the status colours the accent is never closer than 31 for any viewer. With the previous palette the same check gave 0.3 for the reference family and 8.0 for Info, which is why those two moved.

Not covered by any value: text selection and the caret in text editors use JUCE's built-in cyan `#42A2C8` because `DarkLookAndFeel` does not set those colour ids. It is 12.0 from the accent: a slightly greener blue that will look like an off shade of the accent, not the obvious leftover it was beside pink. Setting those ids from the palette is still the fix.

## 6. Waveform trace families, channels 1 to 8

`WaveformDisplay::colourForLane` takes `traceReference` (outputs) or `traceInputL` (inputs) and rotates the hue by 0.045 of a turn (16.2 degrees) per channel index. The palette supplies only channel 1 of each family; the rest follow. Distances are CIEDE2000 with normal vision.

| Ch | Output lane | Contrast on window | To accent family | Input lane | Hue | Contrast on window | To accent family | To amber | To red | To next input |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `#CFD4E6` | 13.3 | 23.8 | `#C7EB75` | 78 | 14.6 | 62.0 | 19.4 | 58.7 | 5.5 |
| 2 | `#D0CFE6` | 12.9 | 25.6 | `#A7EB75` | 95 | 13.9 | 59.5 | 24.8 | 64.5 | 4.2 |
| 3 | `#D6CFE6` | 13.1 | 28.0 | `#87EB75` | 111 | 13.3 | 57.3 | 28.7 | 69.0 | 3.3 |
| 4 | `#DDCFE6` | 13.3 | 30.0 | `#75EB83` | 127 | 13.1 | 54.1 | 30.9 | 70.3 | 5.4 |
| 5 | `#E3CFE6` | 13.4 | 31.3 | `#75EBA2` | 143 | 13.3 | 48.0 | 32.4 | 68.3 | 7.3 |
| 6 | `#E6CFE3` | 13.5 | 31.9 | `#75EBC2` | 159 | 13.5 | 40.5 | 34.6 | 65.0 | 10.0 |
| 7 | `#E6CFDD` | 13.4 | 31.9 | `#75EBE2` | 175 | 13.8 | 32.4 | 38.0 | 59.8 | 12.9 |
| 8 | `#E6CFD7` | 13.4 | 31.7 | `#75D4EB` | 192 | 11.6 | 20.6 | 43.7 | 61.9 |  |

**Outputs.** All eight are the same off-white with a different faint tint: neighbouring outputs differ by delta E2000 3.8 at most, and the two furthest apart by 11.7. In practice outputs can no longer be told from each other by colour, only by their lane label. That is the cost of the move. What it buys: with normal vision no output lane at any of the 64 channel indices comes within 22 of the accent (18 for the worst simulated viewer), 28 of the alarm red or 21 of amber, where the previous blue family reached red at output 10 and the shipping palette does the same. If the owner would rather keep outputs distinguishable, the obvious hued alternative is a violet base (`0xffa780ff`): its first four outputs stay 20.6 or more from the accent with normal vision, but one of them is 1.6 from it for a deuteranope, and the family reaches red by output 7. I did not search hued bases exhaustively.

**Inputs.** Yellow-green through green and teal to cyan (hue 78 to 192). Nothing vanishes: the lowest contrast is 11.6:1. No input lane is closer than 19.4 to amber (input 1) or 20.6 to the accent (input 8); the base hue was set where those two are roughly equal, because moving it either way trades one for the other. Two things the audit already noted still hold. Inputs 1 to 4 are close to each other (see the last column), so on a four-input rig neighbouring lanes are similar greens. And to a red-green dichromat the yellow-green of inputs 1 to 3 is within 4.5 of the warning amber; traces are not status indicators, but it is not a clean separation. The inputs pass within 7.1 of the OK green around lane 6, as the shipping palette's input colour equals its meter green outright.

**Past channel 8** the input rotation keeps going: input 9 is hue 208 and input 10 hue 224, which are the accent's own blues (5.5 and 7.8 from it), and inputs 18, 19, 21, 22 come within 15 of the alarm red or amber. Channel indices run to 63 and the hue wraps every 22 channels, so no base value prevents this; it can only be fixed in code.

## 7. Limitations that a palette cannot fix

- The unfilled part of the output-level slider is drawn in `controlBackground` on `panelBackground` (1.18:1). The filled part and thumb are clear; the remaining travel is faint, as it is today.
- Status-bar captions and ruler numbers are drawn at JUCE font heights of 9 to 9.5, which is an em size of about 8 px. They now have 7.7:1 contrast, but at that size they will not be read from across a room whatever colour they are.
- `traceInputR` and `traceGrid` are defined but no drawing code reads them; their values here are untested in the running app.
- The focus ring (`outlineFocused`) on a latched button is 1.44:1 against the fill, so keyboard focus on a latched button is hard to see (it was 1.12:1 with the pink values). First found by the audit.
- The blue accent has a small footprint in the main window during a normal run: the slider fill and thumb, the Follow tick, and focus rings. It becomes prominent only when Mute or Pause is latched. See `theme_states.png` for those states.
- Everything above is computed or drawn, not measured on the rack machine's display. Dark neutrals this close together (window, panel, alternate row) depend on the monitor's black level.

## 8. Uncommitted work seen in the repo

While this was being made, the working tree gained uncommitted changes from someone else (the palette header was not touched). One of them draws an unmonitored input's meter at 25% alpha with a bold 9.5 "OFF" in `textPrimary` at the top of the bar (`LevelMeterComponent.cpp`). Checked against this palette as that code stood when I read it:

| Case | Colour | Against | Ratio |
|---|---|---|---|
| "OFF" label, bar not reaching the top | `textPrimary` | meterTrough | 15.9 |
| "OFF" label over a dimmed amber bar | `textPrimary` | `#4A4022` (25% over trough) | 8.5 |
| "OFF" label over a dimmed red bar | `textPrimary` | `#4A2422` (25% over trough) | 11.1 |
| dimmed green bar | `#194038` | meterTrough | 1.67 |
| dimmed amber bar | `#4A4022` | meterTrough | 1.87 |
| dimmed red bar | `#4A2422` | meterTrough | 1.43 |

The label passes everywhere. A dimmed bar is 43 to 59 L* darker than a live one, so the two states are not confusable, but a dimmed bar is deliberately faint (under 3:1 against the trough); the three dimmed zone colours remain different hues (`#194038`, `#4A4022`, `#4A2422`). If that code changes before it is committed, these rows need recomputing.
