"""Palette definition + colour maths for the Pink Noise Audio Monitor theme.
Single source for theme.md numbers and the preview mock. Pure stdlib."""
import colorsys, math

# name: (current AARRGGBB, proposed AARRGGBB)
PALETTE = [
 ("windowBackground",   0xff0e1114, 0xff0a0b0d),
 ("panelBackground",    0xff16191d, 0xff15171b),
 ("panelBackgroundAlt", 0xff1a1e23, 0xff1a1c21),
 ("controlBackground",  0xff1c2126, 0xff23262d),
 ("controlHover",       0xff242a31, 0xff2b2e37),
 ("controlPressed",     0xff2b323a, 0xff363a45),
 ("outline",            0xff262c33, 0xff353942),
 ("outlineFocused",     0xff3c586e, 0xff55a4ff),
 ("textPrimary",        0xffd7dbdf, 0xffe8eaee),
 ("textSecondary",      0xff8b939b, 0xffa4aab5),
 ("textDisabled",       0xff525a62, 0xff6e7480),
 ("textOnAccent",       0xff0e1114, 0xff050d1a),
 ("accent",             0xff5b93b8, 0xff3391ff),
 ("accentDim",          0xff3a5d75, 0xff3585e6),
 ("accentBright",       0xff7fb3d3, 0xff55a4ff),
 ("meterGreen",         0xff3fae6a, 0xff3bd1a9),
 ("meterAmber",         0xffd9a53c, 0xffffd452),
 ("meterRed",           0xffd14b4b, 0xffff6450),
 ("meterTrough",        0xff14171b, 0xff0e0f12),
 ("meterPeakTick",      0xffe8ecef, 0xfff6f7f9),
 ("meterRmsOverlay",    0x66ffffff, 0x4dffffff),
 ("traceReference",     0xff5b93b8, 0xffcfd4e6),
 ("traceInputL",        0xff3fae6a, 0xffc7eb75),
 ("traceInputR",        0xffb87fd3, 0xffa8eb75),
 ("traceGrid",          0xff20262c, 0xff1c1e24),
 ("traceCentreLine",    0xff2c343c, 0xff2d3039),
 ("severityInfo",       0xff5b93b8, 0xffbcc2cd),
 ("severityWarning",    0xffd9a53c, 0xffffd452),
 ("severityCritical",   0xffd14b4b, 0xffff6450),
 ("statusRunning",      0xff3fae6a, 0xff3bd1a9),
 ("statusIdle",         0xff525a62, 0xff555a65),
 ("statusPaused",       0xffd9a53c, 0xffffd452),
]
# Values these entries had in the previous (pink-accent) delivery, for the change list in theme.md.
PREVIOUS = {
    "outlineFocused": 0xffd0689f,
    "textOnAccent": 0xff14080e,
    "accent": 0xffec70ae,
    "accentDim": 0xffc85f95,
    "accentBright": 0xffffa6cf,
    "traceReference": 0xff809fff,
    "traceInputL": 0xffb8eb75,
    "traceInputR": 0xff98eb75,
    "severityInfo": 0xff8fb4e3,
}
CUR = {n: c for n, c, p in PALETTE}
NEW = {n: p for n, c, p in PALETTE}

def rgb(argb):  return ((argb >> 16) & 255, (argb >> 8) & 255, argb & 255)
def alpha(argb): return ((argb >> 24) & 255) / 255.0
def hexs(argb):
    a = (argb >> 24) & 255
    return ("#%06X" % (argb & 0xffffff)) if a == 255 else ("#%06X @ %d%%" % (argb & 0xffffff, round(a / 2.55)))
def css(argb):
    r, g, b = rgb(argb); a = alpha(argb)
    return "#%02x%02x%02x" % (r, g, b) if a >= 0.999 else "rgba(%d,%d,%d,%.3f)" % (r, g, b, a)
def lin(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
def unlin(v):
    v = min(1.0, max(0.0, v))
    return round(255 * (12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055))
def lum(c):
    r, g, b = c
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
def contrast(a, b):
    la, lb = lum(a), lum(b)
    if la < lb: la, lb = lb, la
    return (la + 0.05) / (lb + 0.05)
def over(fg, a, bg):
    """sRGB-space alpha blend (what JUCE/CoreGraphics do)."""
    return tuple(round(f * a + b * (1 - a)) for f, b in zip(fg, bg))
def juce_brighter(c, amount):
    """juce::Colour::brighter, as in JUCE 8.0.4 juce_Colour.cpp (float maths, truncated to uint8)."""
    k = 1.0 / (1.0 + amount)
    return tuple(int(255 - k * (255 - v)) for v in c)
def juce_darker(c, amount):
    """juce::Colour::darker, as in JUCE 8.0.4 (truncated to uint8)."""
    k = 1.0 / (1.0 + amount)
    return tuple(int(k * v) for v in c)
def juce_overlaid(dst, src, a):
    """dst.overlaidWith (src.withAlpha (a)) for an opaque dst, as in JUCE 8.0.4 (integer maths)."""
    sa = int(a * 255.0 + 0.5)
    da = ((255 - sa) * 255) // 255
    return tuple(sc + (((d - sc) * da) >> 8) for d, sc in zip(dst, src))
def lab(c):
    r, g, b = (lin(v) for v in c)
    X = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b
    Y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    Z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b
    def f(t): return t ** (1 / 3) if t > 0.008856 else 7.787 * t + 16 / 116
    fx, fy, fz = f(X / 0.95047), f(Y), f(Z / 1.08883)
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))
def dE(a, b):
    return math.dist(lab(a), lab(b))
def dE00(c1, c2):
    """CIEDE2000 colour difference between two sRGB triples."""
    L1, a1, b1 = lab(c1); L2, a2, b2 = lab(c2)
    C1, C2 = math.hypot(a1, b1), math.hypot(a2, b2)
    Cm = (C1 + C2) / 2
    G = 0.5 * (1 - math.sqrt(Cm ** 7 / (Cm ** 7 + 25.0 ** 7)))
    a1p, a2p = (1 + G) * a1, (1 + G) * a2
    C1p, C2p = math.hypot(a1p, b1), math.hypot(a2p, b2)
    h1p = math.degrees(math.atan2(b1, a1p)) % 360 if C1p else 0.0
    h2p = math.degrees(math.atan2(b2, a2p)) % 360 if C2p else 0.0
    dLp, dCp = L2 - L1, C2p - C1p
    if C1p * C2p == 0: dhp = 0.0
    else:
        dhp = h2p - h1p
        if dhp > 180: dhp -= 360
        elif dhp < -180: dhp += 360
    dHp = 2 * math.sqrt(C1p * C2p) * math.sin(math.radians(dhp / 2))
    Lpm, Cpm = (L1 + L2) / 2, (C1p + C2p) / 2
    if C1p * C2p == 0: hpm = h1p + h2p
    elif abs(h1p - h2p) <= 180: hpm = (h1p + h2p) / 2
    else: hpm = (h1p + h2p + 360) / 2 if h1p + h2p < 360 else (h1p + h2p - 360) / 2
    T = (1 - 0.17 * math.cos(math.radians(hpm - 30)) + 0.24 * math.cos(math.radians(2 * hpm))
         + 0.32 * math.cos(math.radians(3 * hpm + 6)) - 0.20 * math.cos(math.radians(4 * hpm - 63)))
    dth = 30 * math.exp(-((hpm - 275) / 25) ** 2)
    Rc = 2 * math.sqrt(Cpm ** 7 / (Cpm ** 7 + 25.0 ** 7))
    Sl = 1 + 0.015 * (Lpm - 50) ** 2 / math.sqrt(20 + (Lpm - 50) ** 2)
    Sc, Sh = 1 + 0.045 * Cpm, 1 + 0.015 * Cpm * T
    Rt = -math.sin(math.radians(2 * dth)) * Rc
    return math.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2 + Rt * (dCp / Sc) * (dHp / Sh))
# Machado, Oliveira & Fernandes 2009, severity 1.0, applied in linear RGB
CVD = {
 "protan": ((0.152286, 1.052583, -0.204868), (0.114503, 0.786281, 0.099216), (-0.003882, -0.048116, 1.051998)),
 "deutan": ((0.367322, 0.860646, -0.227968), (0.280085, 0.672501, 0.047413), (-0.011820, 0.042940, 0.968881)),
}
def sim(c, kind):
    l = [lin(v) for v in c]
    return tuple(unlin(sum(m * v for m, v in zip(row, l))) for row in CVD[kind])
def rot_hue(c, amount):
    """juce::Colour::withRotatedHue: HSB hue += amount (fraction of a turn)."""
    h, s, v = colorsys.rgb_to_hsv(*(x / 255 for x in c))
    r, g, b = colorsys.hsv_to_rgb((h + amount) % 1.0, s, v)
    return (round(r * 255), round(g * 255), round(b * 255))
def hue(c):
    return colorsys.rgb_to_hsv(*(x / 255 for x in c))[0] * 360
def tohex(c): return "#%02X%02X%02X" % tuple(c)

if __name__ == "__main__":
    P = {n: rgb(v) for n, v in NEW.items()}; C = {n: rgb(v) for n, v in CUR.items()}
    def show(title, pairs, pal):
        print("==", title)
        for fg, bg in pairs:
            f = pal[fg] if isinstance(fg, str) else fg[1]; b = pal[bg] if isinstance(bg, str) else bg[1]
            fn = fg if isinstance(fg, str) else fg[0]; bn = bg if isinstance(bg, str) else bg[0]
            print("  %-34s on %-28s %5.2f" % (fn, bn, contrast(f, b)))
    for label, pal in (("CURRENT", C), ("NEW", P)):
        crit0 = over(pal["severityCritical"], 0.10, pal["panelBackground"])
        crit1 = over(pal["severityCritical"], 0.10, pal["panelBackgroundAlt"])
        pairs = [(t, b) for t in ("textPrimary", "textSecondary", "textDisabled") for b in ("windowBackground", "panelBackground", "panelBackgroundAlt", "controlBackground", "controlHover", "controlPressed")]
        pairs += [("textPrimary", ("critRow0", crit0)), ("textPrimary", ("critRow1", crit1))]
        pairs += [("textOnAccent", "accentDim"), ("textOnAccent", "accent"), ("textPrimary", "accentDim")]
        for s in ("severityInfo", "severityWarning", "severityCritical"):
            pairs += [(s, "panelBackground"), (s, "panelBackgroundAlt"), (s, "controlHover")]
        pairs += [("severityCritical", ("critRow0", crit0)), ("severityCritical", ("critRow1", crit1))]
        pairs += [(("FROZEN warn@85%", over(pal["severityWarning"], 0.85, pal["windowBackground"])), "windowBackground"),
                  (("noChan sec@50%", over(pal["textSecondary"], 0.5, pal["windowBackground"])), "windowBackground")]
        for s in ("statusRunning", "statusIdle", "statusPaused", "severityCritical", "outline", "outlineFocused", "accent", "accentBright", "meterTrough"):
            pairs += [(s, "panelBackground")]
        for s in ("meterGreen", "meterAmber", "meterRed", "meterPeakTick", "outline"):
            pairs += [(s, "meterTrough")]
        pairs += [("outline", "windowBackground"), ("traceCentreLine", "windowBackground"), ("controlBackground", "panelBackground"), ("panelBackground", "windowBackground"), ("accent", "controlBackground"), ("outlineFocused", "controlBackground")]
        show(label, pairs, pal)
        print("  trio L*:", {k: round(lab(pal[k])[0], 1) for k in ("meterGreen", "meterAmber", "meterRed", "severityInfo", "accent")})
        for kind in (None, "protan", "deutan"):
            g, a, r, i, ac = (pal[k] if kind is None else sim(pal[k], kind) for k in ("meterGreen", "meterAmber", "meterRed", "severityInfo", "accent"))
            print("  %-7s G-A dE %.1f dL %.1f | A-R dE %.1f dL %.1f | G-R dE %.1f dL %.1f | I-W %.1f I-C %.1f | acc-R %.1f acc-I %.1f  %s" % (
                kind or "normal", dE(g, a), lab(g)[0] - lab(a)[0], dE(a, r), lab(a)[0] - lab(r)[0], dE(g, r), lab(g)[0] - lab(r)[0], dE(i, a), dE(i, r), dE(ac, r), dE(ac, i),
                " ".join(tohex(x) for x in (g, a, r, i, ac))))
        for fam in ("traceReference", "traceInputL"):
            row = []
            for ch in range(8):
                c = rot_hue(pal[fam], 0.045 * ch)
                row.append("%d:%s h%.0f c%.1f" % (ch, tohex(c), hue(c), contrast(c, pal["windowBackground"])))
            print("  ", fam, " | ".join(row))
        # rms overlay on each zone colour
        a = alpha(NEW["meterRmsOverlay"] if label == "NEW" else CUR["meterRmsOverlay"])
        print("  rms wash:", {k: tohex(over((255, 255, 255), a, pal[k])) for k in ("meterGreen", "meterAmber", "meterRed")})
