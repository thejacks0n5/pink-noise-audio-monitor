#!/usr/bin/env python3
"""Writes theme.md (every number computed here, none typed by hand) and
theme_states.png (swatches, control states, lamps, CVD simulation, traces)."""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from palette import (PALETTE, NEW, CUR, PREVIOUS, dE00, rgb, alpha, hexs, css, contrast, over, lab, dE, sim, rot_hue, hue, tohex,
                     juce_brighter, juce_darker, juce_overlaid)
from pngutil import read_png, write_png, crop

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
P = {n: rgb(v) for n, v in NEW.items()}
C = {n: rgb(v) for n, v in CUR.items()}

REASON = {
 "windowBackground":   "Deeper, near-neutral graphite so panels separate from the window ({pw:.2f}:1 instead of {pw_cur:.2f}:1) and the waveform lanes, which sit directly on it, get a blacker field.",
 "panelBackground":    "Neutral graphite with a trace of blue; a touch lighter than the window, dark enough that critical-row red text still clears 4.5:1.",
 "panelBackgroundAlt": "Alternate table rows and table header. Kept close to the panel ({ap:.2f}:1) so zebra striping never competes with the severity tint.",
 "controlBackground":  "Buttons, combos, editors. Lifted further from the panel ({cp:.2f}:1, was {cp_cur:.2f}:1) so controls read as controls from a distance.",
 "controlHover":       "Hover, selected table row, scrollbar thumb. Capped at this lightness because critical red text must stay at 4.5:1 on a selected row.",
 "controlPressed":     "One clear step above hover; also the pressed table-header and scrollbar-thumb fill.",
 "outline":            "Hairlines were {op_cur:.2f}:1 on the panel and vanished across a room; now {op:.2f}:1, still quieter than any text.",
 "outlineFocused":     "Keyboard-focus ring, the same lighter blue as accentBright so it shows on a dark control: {fc:.1f}:1 on a button, {fp:.1f}:1 on the panel (ships at {fp_cur:.1f}:1, below the 3:1 a focus indicator needs). Kept saturated, not pale, so it cannot be taken for a reference trace.",
 "textPrimary":        "Slightly brighter and cooler white for readouts and table cells; {tp:.1f}:1 on the panel.",
 "textSecondary":      "Captions, ruler numbers, table headers. Raised so the small captions hold {sp:.1f}:1 and stay at {spr:.1f}:1 on a pressed header.",
 "textDisabled":       "Disabled labels and the idle lamp. Lifted from {dc_cur:.1f}:1 to {dc:.1f}:1 on a button so a disabled control is still readable as a word.",
 "textOnAccent":       "Text on a latched button. Near-black with a hint of navy; {oa:.1f}:1 on accentDim (the shipping pair is {oa_cur:.1f}:1).",
 "accent":             "The icon's blue, the same value as the middle of its band. Slider fill and toggle ticks; {acp:.1f}:1 on the panel. To red-green dichromats it stays a strong blue.",
 "accentDim":          "Fill of a latched button (Mute, Pause). One step deeper than the accent and still light enough to carry dark text at {oa:.1f}:1.",
 "accentBright":       "Slider thumb. A lighter blue than the accent but deliberately still saturated: a pale ice blue would sit too close to the reference traces and the Info rows.",
 "meterGreen":         "Unchanged by the move to a blue accent. Blue-leaning green, L* {Lg:.0f}; reads as neutral grey to red-green dichromats, which keeps it apart from the two yellowish alarm colours and from the accent, which stays blue for them (delta E2000 {ga:.0f} from the accent with normal vision, {ga_d:.0f} under simulated deuteranopia).",
 "meterAmber":         "Golden amber, L* {La:.0f}, the lightest of the three so it never falls between green and red in lightness.",
 "meterRed":           "Vermilion, L* {Lr:.0f}, the darkest of the three.",
 "meterTrough":        "Unlit bar well, a step darker than the panel so the lit bar has the most room.",
 "meterPeakTick":      "Peak-hold line, near-white, {pt:.1f}:1 on the trough.",
 "meterRmsOverlay":    "Translucent white RMS cap, {rms:.0f}% (ships at {rms_cur:.0f}%). Light enough that the step between the RMS part and the peak part of a bar stays readable ({rs_g:.1f} L* on green, {rs_a:.1f} on amber, {rs_r:.1f} on red). My first proposal was 18%; the audit showed that made the step too small and that the reason I gave for it (keeping a capped red bar away from the accent) did not hold.",
 "traceReference":     "MOVED from cornflower blue to a near-white with a faint cool tint. A blue accent and a blue reference family cannot coexist: the nearest output lane was within delta E2000 {old_ref_n:.0f} of the accent with normal vision and {old_ref_d:.0f} for a deuteranope. At {refS:.0f}% saturation the per-channel hue rotation only tints the white, so every output lane, all 64, stays at least {ref_min:.0f} from the accent for every viewer and can never rotate into red or amber. Reference in white, measured input in colour, is the usual instrument convention.",
 "traceInputL":        "Yellow-green, the base of the input family; moved {in_shift:.0f} degrees toward yellow (hue {hi:.0f}) so that input 8, where the rotation ends, stops at cyan and stays clear of the blue accent.",
 "traceInputR":        "Not referenced by any drawing code today. Set to the colour the input family gives its second channel, so it is consistent if it is used later.",
 "traceGrid":          "Not referenced by any drawing code today. A grid line a step quieter than the centre line, for when a grid is added.",
 "traceCentreLine":    "Zero line in each lane; {cl:.1f}:1 on the window, visible but well under the traces.",
 "severityInfo":       "MOVED from blue to a cool light grey. With a blue accent, a blue Info colour was within delta E2000 {old_info:.0f} of it. Info rows now carry no hue at all: colour in the event log means warning or critical, and the Follow tick and any focus ring beside the table are the only blue there. {info_p:.1f}:1 on the panel.",
 "severityWarning":    "Same value as meterAmber: one amber everywhere.",
 "severityCritical":   "Same value as meterRed: one red everywhere. {cr:.1f}:1 on a tinted critical row, {cs:.1f}:1 on a selected row.",
 "statusRunning":      "Same value as meterGreen.",
 "statusIdle":         "A darker neutral than textDisabled, so a stopped lamp sits further in lightness from the running lamp, which red-green dichromats also see as a grey ({idle_gap:.0f} L* apart under simulated deuteranopia). {ip:.2f}:1 on the panel: visibly an unlit lamp, deliberately below 3:1. Changed from my first proposal on the audit's recommendation.",
 "statusPaused":       "Same value as meterAmber.",
}


def metrics():
    k = contrast
    return dict(
        pw=k(P["panelBackground"], P["windowBackground"]), pw_cur=k(C["panelBackground"], C["windowBackground"]),
        ap=k(P["panelBackgroundAlt"], P["panelBackground"]),
        cp=k(P["controlBackground"], P["panelBackground"]), cp_cur=k(C["controlBackground"], C["panelBackground"]),
        op=k(P["outline"], P["panelBackground"]), op_cur=k(C["outline"], C["panelBackground"]),
        fp=k(P["outlineFocused"], P["panelBackground"]), fp_cur=k(C["outlineFocused"], C["panelBackground"]),
        tp=k(P["textPrimary"], P["panelBackground"]), sp=k(P["textSecondary"], P["panelBackground"]),
        spr=k(P["textSecondary"], P["controlPressed"]),
        dc=k(P["textDisabled"], P["controlBackground"]), dc_cur=k(C["textDisabled"], C["controlBackground"]),
        oa=k(P["textOnAccent"], P["accentDim"]), oa_cur=k(C["textOnAccent"], C["accentDim"]),
        acp=k(P["accent"], P["panelBackground"]),
        Lg=lab(P["meterGreen"])[0], La=lab(P["meterAmber"])[0], Lr=lab(P["meterRed"])[0],
        dEar=dE(P["accent"], P["meterRed"]), pt=k(P["meterPeakTick"], P["meterTrough"]),
        rms=alpha(NEW["meterRmsOverlay"]) * 100, rms_cur=alpha(CUR["meterRmsOverlay"]) * 100,
        hr=hue(P["traceReference"]), hi=hue(P["traceInputL"]), cl=k(P["traceCentreLine"], P["windowBackground"]),
        cr=k(P["severityCritical"], juce_overlaid(P["panelBackgroundAlt"], P["severityCritical"], 0.10)),
        cs=k(P["severityCritical"], P["controlHover"]), ip=k(P["statusIdle"], P["panelBackground"]),
        fc=k(P["outlineFocused"], P["controlBackground"]),
        ga=dE00(P["meterGreen"], P["accent"]), ga_d=dE00(sim(P["meterGreen"], "deutan"), sim(P["accent"], "deutan")),
        old_ref_n=min(fam_dist(rot_hue(rgb(PREVIOUS["traceReference"]), 0.045 * c)) for c in range(8)),
        old_ref_d=min(fam_dist(rot_hue(rgb(PREVIOUS["traceReference"]), 0.045 * c), "deutan") for c in range(8)),
        refS=__import__("colorsys").rgb_to_hsv(*(v / 255 for v in P["traceReference"]))[1] * 100,
        ref_min=min(fam_dist(rot_hue(P["traceReference"], 0.045 * c), kind) for c in range(64) for kind in (None, "protan", "deutan")),
        in_shift=hue(rgb(PREVIOUS["traceInputL"])) - hue(P["traceInputL"]),
        old_info=fam_dist(rgb(PREVIOUS["severityInfo"])), info_p=k(P["severityInfo"], P["panelBackground"]),
        rs_g=rms_step("meterGreen"), rs_a=rms_step("meterAmber"), rs_r=rms_step("meterRed"),
        idle_gap=abs(lab(sim(P["statusRunning"], "deutan"))[0] - lab(sim(P["statusIdle"], "deutan"))[0]))


ACCENT_FAMILY = ("accent", "accentDim", "accentBright", "outlineFocused")


def fam_dist(c, kind=None, with_name=False):
    """Smallest CIEDE2000 distance from colour c to any member of the accent family, optionally as a dichromat sees both."""
    f = (lambda x: x) if kind is None else (lambda x: sim(x, kind))
    best = min((dE00(f(c), f(P[n])), n) for n in ACCENT_FAMILY)
    return best if with_name else best[0]


def rms_step(zone):
    """L* step between the RMS-capped part of a bar and the bare zone colour above it."""
    a = alpha(NEW["meterRmsOverlay"])
    return lab(over((255, 255, 255), a, P[zone]))[0] - lab(P[zone])[0]


brighter, darker = juce_brighter, juce_darker


def backgrounds(pal):
    crit = pal["severityCritical"]
    return {
        "windowBackground": pal["windowBackground"],
        "panelBackground": pal["panelBackground"],
        "panelBackgroundAlt": pal["panelBackgroundAlt"],
        "controlBackground": pal["controlBackground"],
        "controlHover": pal["controlHover"],
        "controlPressed": pal["controlPressed"],
        "button hover (controlBackground.brighter 0.08)": brighter(pal["controlBackground"], 0.08),
        "button down (controlBackground.darker 0.2)": darker(pal["controlBackground"], 0.2),
        "critical row, even (panel + 10% red)": juce_overlaid(pal["panelBackground"], crit, 0.10),
        "critical row, odd (alt + 10% red)": juce_overlaid(pal["panelBackgroundAlt"], crit, 0.10),
        "accentDim": pal["accentDim"],
        "accentDim hover (brighter 0.08)": brighter(pal["accentDim"], 0.08),
        "accentDim down (darker 0.2)": darker(pal["accentDim"], 0.2),
    }


# (text colour, background key, where it is drawn, minimum required or None for exempt)
USES = [
 ("textPrimary", "panelBackground", "status values, meter readout, table cells, labels, dialogs", 4.5),
 ("textPrimary", "panelBackgroundAlt", "table cells on odd rows", 4.5),
 ("textPrimary", "critical row, even (panel + 10% red)", "table cells on a critical row", 4.5),
 ("textPrimary", "critical row, odd (alt + 10% red)", "table cells on a critical row", 4.5),
 ("textPrimary", "controlBackground", "buttons, combo boxes, text editors, slider value box", 4.5),
 ("textPrimary", "button hover (controlBackground.brighter 0.08)", "hovered button", 4.5),
 ("textPrimary", "button down (controlBackground.darker 0.2)", "pressed button", 4.5),
 ("textPrimary", "controlHover", "selected table row, highlighted menu item, hovered status cell", 4.5),
 ("textSecondary", "panelBackground", "status captions (9.5), meter title (13), ruler (9), channel names (9.5)", 4.5),
 ("textSecondary", "panelBackgroundAlt", "table header (13)", 4.5),
 ("textSecondary", "controlHover", "hovered table header", 4.5),
 ("textSecondary", "controlPressed", "pressed table header", 4.5),
 ("textDisabled", "controlBackground", "disabled button label (WCAG exempts disabled controls)", None),
 ("textDisabled", "panelBackground", "disabled toggle label (exempt)", None),
 ("textDisabled", "accentDim", "a button that is disabled while still latched (Mute after Stop Audio); exempt, but see below", None),
 ("textOnAccent", "accentDim", "latched button label (Mute, Resume)", 4.5),
 ("textOnAccent", "accentDim hover (brighter 0.08)", "latched button, hovered", 4.5),
 ("textOnAccent", "accentDim down (darker 0.2)", "latched button while the mouse is held down", 4.5),
 ("severityInfo", "panelBackground", "'Info' in the Severity column (12)", 4.5),
 ("severityInfo", "panelBackgroundAlt", "same, odd row", 4.5),
 ("severityInfo", "controlHover", "same, selected row", 4.5),
 ("severityWarning", "panelBackground", "'Warning' in the Severity column (12)", 4.5),
 ("severityWarning", "panelBackgroundAlt", "same, odd row", 4.5),
 ("severityWarning", "controlHover", "same, selected row", 4.5),
 ("severityCritical", "critical row, even (panel + 10% red)", "'Critical' in the Severity column (12)", 4.5),
 ("severityCritical", "critical row, odd (alt + 10% red)", "same, odd row", 4.5),
 ("severityCritical", "controlHover", "same, selected row", 4.5),
]


def ratio_rows(pal):
    bg = backgrounds(pal)
    out = []
    for fg, b, where, need in USES:
        out.append((fg, b, where, need, contrast(pal[fg], bg[b])))
    w = pal["windowBackground"]
    out.append(("severityWarning @ 85%", "windowBackground", "'FROZEN' tag on the waveform (12)", 4.5,
                contrast(over(pal["severityWarning"], 0.85, w), w)))
    out.append(("textSecondary @ 50%", "windowBackground", "'(no channel selected)' placeholder (11)", 4.5,
                contrast(over(pal["textSecondary"], 0.5, w), w)))
    return out


def verdict(need, r):
    if need is None: return "exempt"
    return "pass" if r >= need else "FAIL"


def trio_table(pal, names):
    rows = []
    for kind in (None, "protan", "deutan"):
        cs = [pal[n] if kind is None else sim(pal[n], kind) for n in names]
        Ls = [lab(c)[0] for c in cs]
        rows.append((kind or "normal vision", cs, Ls,
                     [(abs(Ls[i] - Ls[j]), dE(cs[i], cs[j])) for i, j in ((0, 1), (1, 2), (0, 2))]))
    return rows


def write_md():
    L = []
    A = L.append
    A("# Theme: Graphite + Blue\n")
    A("One dark theme for Pink Noise Audio Monitor. It maps one-to-one onto the existing `pnam::Palette` "
      "constants in `Source/UI/DarkLookAndFeel.h`: every entry is kept, none is added, and applying it means "
      "changing the 32 values and nothing else. The two dB zone thresholds (`meterAmberFromDb`, `meterRedFromDb`) "
      "are not colours and are unchanged.\n")
    A("Every contrast ratio, L*, delta E and hue figure in this file is computed by `tools/gen_theme_docs.py` from "
      "`tools/palette.py` rather than typed in. Contrast is the WCAG 2.x ratio. Semi-transparent colours are composited "
      "first, in sRGB. JUCE's `brighter`, `darker` and `overlaidWith` follow the JUCE 8.0.4 source in the repo's build "
      "tree (truncating integer maths); `withRotatedHue` is a re-implementation that the audit found matches JUCE hex for hex.\n")
    A("## Revision history\n")
    A("1. First delivery: pink accent, matching the first icon.")
    A("2. After the audit (`../design_audit/AUDIT.md`): `meterRmsOverlay` 18%% to %.0f%% alpha and `statusIdle` `#6E7480` to `%s`, "
      "both on the auditor's recommendation. Both are kept." % (alpha(NEW["meterRmsOverlay"]) * 100, tohex(P["statusIdle"])))
    A("3. This revision: the owner asked for blue instead of pink. The accent is now the icon's blue, and because the app "
      "already used blue for the reference traces and for Info rows, those two were moved out of blue. %d entries changed "
      "from revision 2; the other %d did not.\n" % (len(PREVIOUS), len(PALETTE) - len(PREVIOUS)))
    A("| Entry | Revision 2 (pink) | Now | What moved |")
    A("|---|---|---|---|")
    what = {"outlineFocused": "pink to blue (accent)", "textOnAccent": "plum-black to navy-black", "accent": "pink to blue",
            "accentDim": "pink to blue", "accentBright": "pink to blue", "traceReference": "blue to near-white (collision with the accent)",
            "traceInputL": "yellow-green, 8 degrees yellower (room for the accent at the cyan end)",
            "traceInputR": "follows traceInputL (unused entry)", "severityInfo": "blue to cool grey (collision with the accent)"}
    for name, cur, new in PALETTE:
        if name in PREVIOUS:
            assert PREVIOUS[name] != new, name
            A("| `%s` | `%s` | `%s` | %s |" % (name, hexs(PREVIOUS[name]), hexs(new), what[name]))
    A("")
    unchanged = [n for n, c, v in PALETTE if n not in PREVIOUS]
    A("Unchanged from revision 2: %s.\n" % ", ".join("`%s`" % n for n in unchanged))
    M = metrics()
    A("## 1. Palette\n")
    A("| Entry | Current | Proposed | Why |")
    A("|---|---|---|---|")
    for name, cur, new in PALETTE:
        A("| `%s` | `%s` | `%s` | %s |" % (name, hexs(cur), hexs(new), REASON[name].format(**M)))
    A("")
    A("Values as they go into the header (AARRGGBB):\n")
    A("```cpp")
    for name, cur, new in PALETTE:
        A("static constexpr juce::uint32 %-18s = 0x%08x;" % (name, new))
    A("```\n")
    A("Comments in the header that the new values make stale, for whoever integrates this: the file banner "
      "(\"desaturated steel-blue accent\"), the `// -- Accent (desaturated steel blue)` heading, and the trailing "
      "`// accent`, `// green`, `// muted violet` notes on the three trace entries. `UI_DESIGN.md` section 2 repeats "
      "the old hex values in a table.\n")

    A("## 2. Text contrast\n")
    A("Requirement: 4.5:1 for body text, 3:1 for large status text. Nothing in this UI is large by the WCAG definition "
      "(the biggest text is the 16-unit button label), so every row is held to 4.5:1.\n")
    A("| Text colour | Drawn on | Where | Ratio | 4.5:1 | Current palette |")
    A("|---|---|---|---|---|---|")
    cur_rows = {(r[0], r[1]): r[4] for r in ratio_rows(C)}
    fails = []
    for fg, b, where, need, r in ratio_rows(P):
        v = verdict(need, r)
        if v == "FAIL": fails.append((fg, b, r))
        name = "`%s`" % fg if "@" not in fg else "`%s` at %s alpha" % tuple(fg.split(" @ "))
        A("| %s | %s | %s | **%.2f** | %s | %.2f |" % (name, b, where, r, v, cur_rows[(fg, b)]))
    A("")
    A("Two rows fail and neither can be fixed by a palette value alone, and one exempt row is unreadable in practice:\n")
    A("- **`textSecondary` at 50%% alpha** (`WaveformDisplay.cpp`, the \"(no channel selected)\" hint). The code halves the "
      "alpha, so `textSecondary` would have to be as light as `textPrimary` to reach 4.5:1, which would flatten the text "
      "hierarchy everywhere else. It is an empty-state hint, not data. Dropping the `.withAlpha (0.5f)` would give %.2f:1."
      % contrast(P["textSecondary"], P["windowBackground"]))
    A("- **`textOnAccent` on a latched button while the mouse button is held down.** `drawButtonBackground` darkens the "
      "fill by `darker (0.2f)` for that moment. Passing it would need `accentDim` as light as `accent` itself, which "
      "would make a latched Mute button glare. The resting and hovered states pass.\n")
    A("- **`textDisabled` on `accentDim` (%.2f:1).** Found by the audit, not by me. From the code, Mute can stay latched "
      "when audio stops, and the button is then both disabled and toggled, so its label is drawn in `textDisabled` on the "
      "blue fill. Disabled controls are exempt from WCAG, but the word cannot be read. It is %.2f:1 today. It needs a code "
      "change (a different label colour for a disabled, latched button), not a value." % (
          contrast(P["textDisabled"], P["accentDim"]), contrast(C["textDisabled"], C["accentDim"])))
    A("")
    cur_fail = [(fg, b, r) for fg, b, where, need, r in ratio_rows(C) if verdict(need, r) == "FAIL"]
    A("For comparison the shipping palette fails %d of these rows, including the latched-button label (%.2f:1) and "
      "`Critical` on a critical row (%.2f:1).\n" % (len(cur_fail), contrast(C["textOnAccent"], C["accentDim"]),
                                                   contrast(C["severityCritical"], backgrounds(C)["critical row, odd (alt + 10% red)"])))

    A("## 3. Non-text contrast\n")
    A("| Element | Against | Ratio | Note |")
    A("|---|---|---|---|")
    nt = [("statusRunning lamp", P["statusRunning"], "panelBackground", "7 px lamp"),
          ("statusPaused lamp", P["statusPaused"], "panelBackground", ""),
          ("severityCritical lamp", P["severityCritical"], "panelBackground", "Device error / Write error"),
          ("statusIdle lamp", P["statusIdle"], "panelBackground", "an unlit lamp; deliberately below 3:1 (see the palette table)"),
          ("meterGreen fill", P["meterGreen"], "meterTrough", ""),
          ("meterAmber fill", P["meterAmber"], "meterTrough", ""),
          ("meterRed fill / clip lamp", P["meterRed"], "meterTrough", ""),
          ("meterPeakTick", P["meterPeakTick"], "meterTrough", "drawn above the lit bar"),
          ("accent (slider fill, toggle tick)", P["accent"], "controlBackground", ""),
          ("accentBright (slider thumb)", P["accentBright"], "panelBackground", ""),
          ("outlineFocused (focus ring)", P["outlineFocused"], "controlBackground", "3:1 needed"),
          ("outline (hairlines)", P["outline"], "panelBackground", "decorative; deliberately quiet"),
          ("controlBackground (button body)", P["controlBackground"], "panelBackground", "buttons are identified by their label, not this edge"),
          ("controlBackground (slider track)", P["controlBackground"], "panelBackground", "the unfilled track is faint; see limitations"),
          ("traceCentreLine", P["traceCentreLine"], "windowBackground", "decorative")]
    for name, c, bg, note in nt:
        A("| %s | %s | %.2f | %s |" % (name, bg, contrast(c, P[bg]), note))
    A("")

    A("## 4. OK / warning / critical under red-green colour-vision deficiency\n")
    A("Simulated with the Machado, Oliveira and Fernandes (2009) matrices at full severity (protanopia, deuteranopia), "
      "applied in linear RGB. The matrices are in `tools/palette.py`; I entered them from memory and checked only that each "
      "row sums to one, so treat them as worth a second look. A simulation is also not a test with colour-deficient viewers. "
      "L* is CIE lightness; delta E is CIE76. One green, one amber and one red are used "
      "everywhere (meter zones, status lamps, severity stripes and words), so this one table covers all three uses.\n")
    A("| Viewer | Green (OK) | Amber (warning) | Red (critical) | L* green / amber / red | Green-amber: dL*, dE | Amber-red: dL*, dE | Green-red: dL*, dE |")
    A("|---|---|---|---|---|---|---|---|")
    worst = 99
    for label, cs, Ls, pairs in trio_table(P, ("meterGreen", "meterAmber", "meterRed")):
        worst = min(worst, min(p[0] for p in pairs))
        A("| %s | `%s` | `%s` | `%s` | %.0f / %.0f / %.0f | %.1f, %.0f | %.1f, %.0f | %.1f, %.0f |" % (
            label, tohex(cs[0]), tohex(cs[1]), tohex(cs[2]), Ls[0], Ls[1], Ls[2],
            pairs[0][0], pairs[0][1], pairs[1][0], pairs[1][1], pairs[2][0], pairs[2][1]))
    A("")
    cur_t = trio_table(C, ("meterGreen", "meterAmber", "meterRed"))
    cur_worst = min(min(p[0] for p in row[3]) for row in cur_t)
    A("The lightness order is the same for all three viewers: red darkest, green in the middle, amber lightest. "
      "With normal vision every pair is at least %.1f L* apart. Under simulation the smallest gap is **%.1f L*** "
      "(the shipping palette's smallest is %.1f L*). That is a real lightness difference but not a large one, and I could "
      "not make it larger: red is also 12 px text and must keep 4.5:1 on a tinted row, which sets a floor on how dark it "
      "can go, and amber cannot go much lighter and still be amber. What carries the rest is chroma: to a red-green "
      "dichromat the green turns neutral grey while amber stays a vivid yellow and red becomes a dark olive, so the "
      "smallest simulated colour difference between any pair is delta E %.0f.\n" % (
          min(p[0] for p in trio_table(P, ("meterGreen", "meterAmber", "meterRed"))[0][3]), worst, cur_worst,
          min(min(p[1] for p in row[3]) for row in trio_table(P, ("meterGreen", "meterAmber", "meterRed")))))
    A("Colour is never the only signal in the UI as written: each status lamp sits beside a word (Running, Paused, "
      "Device error), each severity stripe has the word in the Severity column, and the meter also shows level by height.\n")
    A("The severity trio in the event log (info / warning / critical):\n")
    A("| Viewer | Info | Warning | Critical | L* | Info-warning dE | Info-critical dE | Warning-critical dL* |")
    A("|---|---|---|---|---|---|---|---|")
    for label, cs, Ls, pairs in trio_table(P, ("severityInfo", "severityWarning", "severityCritical")):
        A("| %s | `%s` | `%s` | `%s` | %.0f / %.0f / %.0f | %.0f | %.0f | %.1f |" % (
            label, tohex(cs[0]), tohex(cs[1]), tohex(cs[2]), Ls[0], Ls[1], Ls[2], pairs[0][1], pairs[2][1], pairs[1][0]))
    A("")
    A("Info is now a neutral grey, so it is separated from warning and critical by having no hue at all. To a red-green "
      "dichromat the OK green is also a grey (`%s`, against Info's `%s`), but the two never share a role: Info is a stripe and a "
      "word in the event log, green is a lamp and a meter fill.\n" % (tohex(sim(P["meterGreen"], "deutan")), tohex(sim(P["severityInfo"], "deutan"))))

    A("## 5. Is the blue accent confusable with anything else?\n")
    A("The accent family is `accent` (`%s`), `accentDim` (`%s`), and `accentBright` and `outlineFocused` (both `%s`). The table gives, "
      "for each thing it must not be mistaken for, the smallest CIEDE2000 distance to any member of that family, with normal "
      "vision and as simulated for protanopia and deuteranopia. For scale: the audit treated distances around 20 as clearly "
      "apart and distances near 10 or below as collisions.\n" % (tohex(P["accent"]), tohex(P["accentDim"]), tohex(P["accentBright"])))
    A("| Against | Colour | Normal | Protan | Deutan | Closest accent entry (normal) |")
    A("|---|---|---|---|---|---|")
    outs = [rot_hue(P["traceReference"], 0.045 * c) for c in range(64)]
    ins = [rot_hue(P["traceInputL"], 0.045 * c) for c in range(8)]
    def worst_of(cols, kind=None):
        return min((fam_dist(c, kind), i) for i, c in enumerate(cols))
    rows5 = []
    for label, cols in (("Output lanes 1 to 8, closest", outs[:8]), ("Output lanes 1 to 64, closest", outs), ("Input lanes 1 to 8, closest", ins)):
        d, i = worst_of(cols)
        rows5.append(("%s (lane %d)" % (label, i + 1), cols[i], d, worst_of(cols, "protan")[0], worst_of(cols, "deutan")[0], fam_dist(cols[i], None, True)[1]))
    for label, name in (("Info rows", "severityInfo"), ("OK green", "meterGreen"), ("Warning amber", "meterAmber"), ("Critical red", "meterRed"),
                        ("Idle lamp", "statusIdle"), ("Body text", "textPrimary"), ("Caption text", "textSecondary")):
        c = P[name]
        rows5.append((label + " (`%s`)" % name, c, fam_dist(c), fam_dist(c, "protan"), fam_dist(c, "deutan"), fam_dist(c, None, True)[1]))
    for label, c, n, pr, de, who in rows5:
        A("| %s | `%s` | %.1f | %.1f | %.1f | `%s` |" % (label, tohex(c), n, pr, de, who))
    A("")
    smallest = min(min(r[2], r[3], r[4]) for r in rows5)
    in_d, in_i = worst_of(ins)
    A("The smallest figure anywhere in the table is %.1f. The closest thing to the accent with normal vision is input lane %d (%.1f), "
      "the cyan end of the input family against the lighter accent blue; inputs 1 to 6 are all above %.0f. Against the status colours the "
      "accent is never closer than %.0f for any viewer. With the previous palette the same check gave %.1f for the reference "
      "family and %.1f for Info, which is why those two moved.\n" % (
          smallest, in_i + 1, in_d, min(fam_dist(c) for c in ins[:6]) - 0.5,
          min(fam_dist(P[n], kd) for n in ("meterGreen", "meterAmber", "meterRed") for kd in (None, "protan", "deutan")) - 0.5,
          min(fam_dist(rot_hue(rgb(PREVIOUS["traceReference"]), 0.045 * c), kd) for c in range(8) for kd in (None, "protan", "deutan")),
          min(fam_dist(rgb(PREVIOUS["severityInfo"]), kd) for kd in (None, "protan", "deutan"))))
    A("Not covered by any value: text selection and the caret in text editors use JUCE's built-in cyan `#42A2C8` because "
      "`DarkLookAndFeel` does not set those colour ids. It is %.1f from the accent: a slightly greener blue that will look like an "
      "off shade of the accent, not the obvious leftover it was beside pink. Setting those ids from the palette is still the fix.\n" % fam_dist((0x42, 0xA2, 0xC8)))

    A("## 6. Waveform trace families, channels 1 to 8\n")
    A("`WaveformDisplay::colourForLane` takes `traceReference` (outputs) or `traceInputL` (inputs) and rotates the hue by "
      "0.045 of a turn (16.2 degrees) per channel index. The palette supplies only channel 1 of each family; the rest follow. "
      "Distances are CIEDE2000 with normal vision.\n")
    A("| Ch | Output lane | Contrast on window | To accent family | Input lane | Hue | Contrast on window | To accent family | To amber | To red | To next input |")
    A("|---|---|---|---|---|---|---|---|---|---|---|")
    for ch in range(8):
        r, i = outs[ch], ins[ch]
        nxt = "%.1f" % dE00(i, ins[ch + 1]) if ch < 7 else ""
        A("| %d | `%s` | %.1f | %.1f | `%s` | %.0f | %.1f | %.1f | %.1f | %.1f | %s |" % (
            ch + 1, tohex(r), contrast(r, P["windowBackground"]), fam_dist(r), tohex(i), hue(i), contrast(i, P["windowBackground"]),
            fam_dist(i), dE00(i, P["meterAmber"]), dE00(i, P["meterRed"]), nxt))
    A("")
    A("**Outputs.** All eight are the same off-white with a different faint tint: neighbouring outputs differ by delta E2000 "
      "%.1f at most, and the two furthest apart by %.1f. In practice outputs can no longer be told from each other by colour, only by "
      "their lane label. That is the cost of the move. What it buys: with normal vision no output lane at any of the 64 channel "
      "indices comes within %.0f of the accent (%.0f for the worst simulated viewer), %.0f of the alarm red or %.0f of amber, where the "
      "previous blue family reached red at output 10 and the shipping palette does the same. If the owner would rather keep "
      "outputs distinguishable, the obvious hued alternative is a violet base (`0xffa780ff`): its first four outputs stay %.1f or more from "
      "the accent with normal vision, but one of them is %.1f from it for a deuteranope, and the family reaches red by output 7. "
      "I did not search hued bases exhaustively.\n" % (
          max(dE00(outs[i], outs[i + 1]) for i in range(7)), max(dE00(a, b) for a in outs[:8] for b in outs[:8]),
          min(fam_dist(c) for c in outs) - 0.5, min(fam_dist(c, kd) for c in outs for kd in ("protan", "deutan")) - 0.5,
          min(dE00(c, P["meterRed"]) for c in outs) - 0.5, min(dE00(c, P["meterAmber"]) for c in outs) - 0.5,
          min(fam_dist(rot_hue((0xA7, 0x80, 0xFF), 0.045 * c)) for c in range(4)),
          min(fam_dist(rot_hue((0xA7, 0x80, 0xFF), 0.045 * c), "deutan") for c in range(4))))
    A("**Inputs.** Yellow-green through green and teal to cyan (hue %.0f to %.0f). Nothing vanishes: the lowest contrast is %.1f:1. "
      "No input lane is closer than %.1f to amber (input 1) or %.1f to the accent (input 8); the base hue was set where those two "
      "are roughly equal, because moving it either way trades one for the other. Two things the audit already noted still hold. "
      "Inputs 1 to 4 are close to each other (see the last column), so on a four-input rig neighbouring lanes are similar greens. "
      "And to a red-green dichromat the yellow-green of inputs 1 to 3 is within %.1f of the warning amber; traces are not status "
      "indicators, but it is not a clean separation. The inputs pass within %.1f of the OK green around lane %d, as the shipping "
      "palette's input colour equals its meter green outright.\n" % (
          hue(ins[0]), hue(ins[7]), min(contrast(c, P["windowBackground"]) for c in ins),
          min(dE00(c, P["meterAmber"]) for c in ins), min(fam_dist(c) for c in ins),
          min(dE00(sim(c, "deutan"), sim(P["meterAmber"], "deutan")) for c in ins[:3]),
          min(dE00(c, P["meterGreen"]) for c in ins), 1 + min(range(8), key=lambda j: dE00(ins[j], P["meterGreen"]))))
    ins22 = [rot_hue(P["traceInputL"], 0.045 * c) for c in range(22)]
    alarm = [i + 1 for i, c in enumerate(ins22) if min(dE00(c, P["meterRed"]), dE00(c, P["meterAmber"])) < 15]
    A("**Past channel 8** the input rotation keeps going: input 9 is hue %.0f and input 10 hue %.0f, which are the accent's own "
      "blues (%.1f and %.1f from it), and inputs %s come within 15 of the alarm red or amber. Channel indices run to 63 and the hue "
      "wraps every 22 channels, so no base value prevents this; it can only be fixed in code.\n" % (
          hue(ins22[8]), hue(ins22[9]), fam_dist(ins22[8]), fam_dist(ins22[9]), ", ".join(str(i) for i in alarm)))

    A("## 7. Limitations that a palette cannot fix\n")
    A("- The unfilled part of the output-level slider is drawn in `controlBackground` on `panelBackground` (%.2f:1). The filled part and thumb are clear; the remaining travel is faint, as it is today." % contrast(P["controlBackground"], P["panelBackground"]))
    A("- Status-bar captions and ruler numbers are drawn at JUCE font heights of 9 to 9.5, which is an em size of about 8 px. "
      "They now have %.1f:1 contrast, but at that size they will not be read from across a room whatever colour they are." % contrast(P["textSecondary"], P["panelBackground"]))
    A("- `traceInputR` and `traceGrid` are defined but no drawing code reads them; their values here are untested in the running app.")
    A("- The focus ring (`outlineFocused`) on a latched button is %.2f:1 against the fill, so keyboard focus on a latched button is hard to see (it was 1.12:1 with the pink values). First found by the audit." % contrast(P["outlineFocused"], P["accentDim"]))
    A("- The blue accent has a small footprint in the main window during a normal run: the slider fill and thumb, the Follow tick, and focus rings. It becomes prominent only when Mute or Pause is latched. See `theme_states.png` for those states.")
    A("- Everything above is computed or drawn, not measured on the rack machine's display. Dark neutrals this close together (window, panel, alternate row) depend on the monitor's black level.")
    A("")
    A("## 8. Uncommitted work seen in the repo\n")
    A("While this was being made, the working tree gained uncommitted changes from someone else (the palette header was not "
      "touched). One of them draws an unmonitored input's meter at 25% alpha with a bold 9.5 \"OFF\" in `textPrimary` at the "
      "top of the bar (`LevelMeterComponent.cpp`). Checked against this palette as that code stood when I read it:\n")
    A("| Case | Colour | Against | Ratio |")
    A("|---|---|---|---|")
    tr = P["meterTrough"]
    dim = {n: over(P[n], 0.25, tr) for n in ("meterGreen", "meterAmber", "meterRed")}
    A("| \"OFF\" label, bar not reaching the top | `textPrimary` | meterTrough | %.1f |" % contrast(P["textPrimary"], tr))
    for n in ("meterAmber", "meterRed"):
        A("| \"OFF\" label over a dimmed %s bar | `textPrimary` | `%s` (25%% over trough) | %.1f |"
          % (n.replace("meter", "").lower(), tohex(dim[n]), contrast(P["textPrimary"], dim[n])))
    for n in ("meterGreen", "meterAmber", "meterRed"):
        A("| dimmed %s bar | `%s` | meterTrough | %.2f |" % (n.replace("meter", "").lower(), tohex(dim[n]), contrast(dim[n], tr)))
    A("")
    A("The label passes everywhere. A dimmed bar is %.0f to %.0f L* darker than a live one, so the two states are not "
      "confusable, but a dimmed bar is deliberately faint (under 3:1 against the trough); the three dimmed zone colours "
      "remain different hues (`%s`, `%s`, `%s`). If that code changes before it is committed, these rows need recomputing.\n" % (
          min(lab(P[n])[0] - lab(dim[n])[0] for n in dim), max(lab(P[n])[0] - lab(dim[n])[0] for n in dim),
          tohex(dim["meterGreen"]), tohex(dim["meterAmber"]), tohex(dim["meterRed"])))
    open(os.path.join(ROOT, "theme.md"), "w").write("\n".join(L))
    return fails


def states_png():
    def chip(c, w=44, h=22, extra=""):
        return '<span class="chip" style="width:%dpx;height:%dpx;background:%s;%s"></span>' % (w, h, c, extra)
    sw = ""
    for name, cur, new in PALETTE:
        sw += ('<div class="sw">%s%s<b>%s</b><i>%s</i></div>' % (chip(css(cur), 26), chip(css(new), 44), name, hexs(new)))
    def btn(label, fill, txt, edge):
        return '<span class="btn" style="background:%s;color:%s;border-color:%s">%s</span>' % (fill, txt, edge, label)
    p = {n: css(v) for n, v in NEW.items()}
    states = (btn("Normal", p["controlBackground"], p["textPrimary"], p["outline"])
              + btn("Hover", tohex(brighter(P["controlBackground"], 0.08)), p["textPrimary"], p["outline"])
              + btn("Pressed", tohex(darker(P["controlBackground"], 0.2)), p["textPrimary"], p["outline"])
              + btn("Focused", p["controlBackground"], p["textPrimary"], p["outlineFocused"])
              + btn("Disabled", p["controlBackground"], p["textDisabled"], p["outline"])
              + btn("Mute (latched)", p["accentDim"], p["textOnAccent"], p["outline"])
              + btn("Resume (latched, hover)", tohex(brighter(P["accentDim"], 0.08)), p["textOnAccent"], p["outline"]))
    def lamp(col, cap, val):
        return ('<span class="cell"><span class="cap"><span class="lamp" style="background:%s"></span>%s</span>'
                '<span class="val">%s</span></span>' % (col, cap, val))
    lamps = (lamp(p["statusIdle"], "AUDIO", "Stopped") + lamp(p["statusPaused"], "AUDIO", "Starting...")
             + lamp(p["statusRunning"], "AUDIO", "Running") + lamp(p["severityCritical"], "AUDIO", "Device error")
             + lamp(p["statusRunning"], "LOG", "Logging") + lamp(p["statusPaused"], "LOG", "Paused")
             + lamp(p["severityCritical"], "LOG", "Write error"))
    trio = ""
    for kind, label in ((None, "Normal vision"), ("protan", "Protanopia (simulated)"), ("deutan", "Deuteranopia (simulated)")):
        cells = ""
        for n, word in (("meterGreen", "OK"), ("meterAmber", "Warning"), ("meterRed", "Critical"), ("severityInfo", "Info"), ("accent", "Accent")):
            c = P[n] if kind is None else sim(P[n], kind)
            cells += '<span class="tri"><span class="chip" style="width:74px;height:30px;background:%s"></span><i>%s &middot; L* %.0f</i></span>' % (tohex(c), word, lab(c)[0])
        trio += '<div class="trow"><span class="tl">%s</span>%s</div>' % (label, cells)
    fam = ""
    for name, label in (("traceReference", "Output lanes 1-8"), ("traceInputL", "Input lanes 1-8")):
        lines = "".join('<span class="tr"><span style="background:%s"></span><span style="background:%s;margin-top:9px"></span><i>%d</i></span>'
                        % (tohex(rot_hue(P[name], 0.045 * ch)), tohex(rot_hue(P[name], 0.045 * ch)), ch + 1) for ch in range(8))
        fam += '<div class="trow"><span class="tl">%s</span><span class="lane">%s</span></div>' % (label, lines)
    html = """<!doctype html><meta charset="utf-8"><style>
body{margin:0;width:1120px;background:%(win)s;color:%(tp)s;font:11px/1.3 'Lucida Grande','Helvetica Neue',Helvetica,Arial,sans-serif}
section{background:%(panel)s;margin:8px;padding:12px 14px 14px}
h3{margin:0 0 10px;font-size:10px;font-weight:normal;letter-spacing:.08em;text-transform:uppercase;color:%(ts)s}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:7px 14px}
.sw{display:flex;align-items:center;gap:6px;white-space:nowrap}
.chip{display:inline-block;border:1px solid %(ol)s;box-sizing:border-box;flex:none}
.sw b{font-weight:normal}.sw i{font-style:normal;color:%(ts)s;font-family:Menlo,monospace;font-size:10px;margin-left:auto}
.btn{display:inline-block;border:1px solid;border-radius:4px;padding:0 16px;height:36px;line-height:36px;font-size:13px;margin-right:6px}
.cell{display:inline-block;margin-right:26px}.cap{display:block;font-size:8px;color:%(ts)s}.val{font-size:11px}
.lamp{display:inline-block;width:7px;height:7px;border-radius:50%%;margin-right:4px}
.trow{display:flex;align-items:center;margin-bottom:8px}.tl{width:170px;color:%(ts)s}
.tri{display:inline-flex;flex-direction:column;margin-right:14px}.tri i{font-style:normal;color:%(ts)s;font-size:10px;margin-top:3px}
.lane{background:%(win)s;padding:8px 10px;display:inline-flex;gap:12px}
.tr{display:inline-flex;flex-direction:column;width:84px}.tr span{height:1px;display:block}.tr i{font-style:normal;color:%(ts)s;font-size:9px;margin-top:5px}
.note{color:%(ts)s;margin:0 0 10px}
</style><body>
<section><h3>Palette &mdash; small chip: shipping value &middot; large chip: proposed value</h3><div class="grid">%(sw)s</div></section>
<section><h3>Button states (fills as DarkLookAndFeel::drawButtonBackground computes them)</h3>%(states)s</section>
<section><h3>Status lamps</h3>%(lamps)s</section>
<section><h3>Alarm colours, info and accent as seen with red-green colour-vision deficiency (Machado 2009, full severity)</h3>%(trio)s</section>
<section><h3>Waveform trace families (base value rotated +16.2&deg; of hue per channel, as WaveformDisplay::colourForLane does); 1 px lines on windowBackground</h3>%(fam)s</section>
</body>""" % dict(win=p["windowBackground"], tp=p["textPrimary"], ts=p["textSecondary"], panel=p["panelBackground"], ol=p["outline"],
                  sw=sw, states=states, lamps=lamps, trio=trio, fam=fam)
    page = os.path.join(ROOT, "extras", "theme_states.html"); sheet = os.path.join(ROOT, "extras", "theme_states_raw.png")
    open(page, "w").write(html)
    Hh = 800
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--window-size=1200,%d" % (Hh + 100), "--screenshot=" + sheet, "file://" + page],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    w, h, rows = read_png(sheet)
    bgc = bytes(P["windowBackground"]) + b"\xff"
    last = max(y for y in range(h) if any(rows[y][x * 4:x * 4 + 4] != b"\xff\xff\xff\xff" and rows[y][x * 4:x * 4 + 3] != bgc[:3]
                                          for x in range(0, 1120, 16)))
    write_png(os.path.join(ROOT, "theme_states.png"), 1120, last + 9, crop(rows, 0, 0, 1120, last + 9))
    os.remove(sheet)


if __name__ == "__main__":
    fails = write_md()
    print("FAIL rows:", fails)
    states_png()
    print("ok")
