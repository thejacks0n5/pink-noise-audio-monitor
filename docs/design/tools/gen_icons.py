#!/usr/bin/env python3
"""Generates the three icon concepts as SVG (1024 canvas) and renders PNGs.

Every concept has one full master (used for 64 px and up) and two pixel-fitted
small masters (32 px and 16 px) whose edges sit on the target pixel grid.
Rendering: headless Google Chrome (already installed), one screenshot per size.
Pure stdlib; no downloads.
"""
import math, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import apple_shape
OUT = os.path.join(os.path.dirname(HERE), "icons")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
SIZES = [16, 32, 64, 128, 256, 512, 1024]

# Brand colours (kept in step with theme.md: accent / accentBright / amber / window)
PINK, PINK_HI, PINK_LO = "#EC70AE", "#FFA6CF", "#D4508F"
AMBER, WHITE = "#FFD452", "#F6F7F9"

# Colour schemes for concept A. hi/mid/lo are the band's top/middle/bottom gradient
# stops; peaks is the darker outer noise layer; line is the collapsed signal in the
# gap; flat16 is the single flat tone the 16 px master uses; line32 the gap line at 32 px.
# flat16 was chosen by looking at the enlarged 16 px renders, not derived: steel needs a
# lighter, bluer tone than its mid stop or it reads as grey at that size, and the deeper
# blue of scheme 3 is lifted slightly so a 4 px band keeps its brightness on a dark Dock.
SCHEMES = {
    "pink":  dict(label="original pink (superseded)", hi="#FFA6CF", mid="#EC70AE", lo="#D4508F", peaks="#D4508F",
                  line="#FFA6CF", flat16="#EC70AE", line32="#FFA6CF", amber=AMBER),
    "blue1": dict(label="1  Electric", hi="#93DDFF", mid="#1FB0FF", lo="#0A7EE0", peaks="#0A7EE0",
                  line="#B4E8FF", flat16="#1FB0FF", line32="#B4E8FF", amber=AMBER),
    "blue2": dict(label="2  Steel", hi="#D5E5F2", mid="#8DB0CC", lo="#5F86A8", peaks="#567A9A",
                  line="#E2EEF7", flat16="#9FC3E3", line32="#F0F6FB", amber=AMBER),
    "blue3": dict(label="3  Honed", hi="#A8D0FF", mid="#3391FF", lo="#1B62DE", peaks="#1B62DE",
                  line="#C4DFFF", flat16="#3F98FF", line32="#C4DFFF", amber=AMBER),
}
FINAL_SCHEME = "blue3"      # what icons/A and final/ are built from
BODY_HI, BODY_LO = "#353740", "#1D1F25"
INK = "#15161B"


def squircle(x0, y0, x1, y1, n=5.0, pts=180):
    """Superellipse outline (continuous-curvature corners), as an SVG path."""
    cx, cy, a, b = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    out = []
    for i in range(pts):
        t = 2 * math.pi * i / pts
        c, s = math.cos(t), math.sin(t)
        x = cx + a * math.copysign(abs(c) ** (2 / n), c)
        y = cy + b * math.copysign(abs(s) ** (2 / n), s)
        out.append("%.1f,%.1f" % (x, y))
    return "M" + " L".join(out) + "Z"


def body(size, hi=BODY_HI, lo=BODY_LO, rim="#FFFFFF", rim_op=0.22, template=False):
    """Icon body. 64 px and up: 824x824 at the standard 100 px margin with the
    template drop shadow. 32/16 px: snapped to whole pixels (26 px / 14 px) and
    given a one-pixel rim so a dark body still separates from a dark Dock.

    template=True (concept A, the shipping icon) draws the 64 px-and-up body as
    Apple's exact template shape (continuous corners, radius 185.4) instead of
    the superellipse approximation. macOS 27 puts an icon whose large sizes miss
    that shape onto a black plate. Concepts B and C keep the approximation; they
    are comparison pieces only."""
    if size >= 64:
        m0, m1, rw = 100, 924, 10
        shadow = ('<filter id="sh" x="-20%" y="-20%" width="140%" height="140%">'
                  '<feGaussianBlur in="SourceAlpha" stdDeviation="11"/><feOffset dy="12"/>'
                  '<feComponentTransfer><feFuncA type="linear" slope="0.32"/></feComponentTransfer>'
                  '<feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge></filter>')
        filt = ' filter="url(#sh)"'
    else:
        u = 1024 / size
        m = 3 * u if size == 32 else 1 * u
        m0, m1, rw = m, 1024 - m, u
        shadow, filt = "", ""
    if template and size >= 64:
        d, inset = apple_shape.BODY, apple_shape.RIM
    else:
        d = squircle(m0, m0, m1, m1)
        inset = squircle(m0 + rw / 2, m0 + rw / 2, m1 - rw / 2, m1 - rw / 2)
    defs = ('<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>'
            '<linearGradient id="rim" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="%s" stop-opacity="%.2f"/>'
            '<stop offset="0.55" stop-color="%s" stop-opacity="%.2f"/>'
            '<stop offset="1" stop-color="%s" stop-opacity="%.2f"/></linearGradient>'
            '<clipPath id="clip"><path d="%s"/></clipPath>%s') % (
                hi, lo, rim, rim_op, rim, rim_op * 0.45, rim, rim_op * 0.5, d, shadow)
    shape = '<path d="%s" fill="url(#bg)"%s/>' % (d, filt)
    rim_svg = '<path d="%s" fill="none" stroke="url(#rim)" stroke-width="%.1f"/>' % (inset, rw)
    return defs, shape, rim_svg


def wrap(defs, content, title):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">\n'
            '<title>%s</title>\n<defs>%s</defs>\n%s\n</svg>\n') % (title, defs, content)


def rect(x, y, w, h, fill, r=0, op=1.0):
    extra = ' rx="%.1f"' % r if r else ""
    extra += ' opacity="%.2f"' % op if op < 1 else ""
    return '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s"%s/>' % (x, y, w, h, fill, extra)


class Rng:
    """xorshift64* - the same family of generator the app uses for its noise."""
    def __init__(self, seed): self.s = seed & 0xFFFFFFFFFFFFFFFF
    def next(self):
        s = self.s
        s ^= s >> 12; s ^= (s << 25) & 0xFFFFFFFFFFFFFFFF; s ^= s >> 27
        self.s = s
        return ((s * 0x2545F4914F6CDD1D) & 0xFFFFFFFFFFFFFFFF) / 2 ** 64


# ----------------------------------------------------------------------------
# Concept A - "Dropout": a band of noise that collapses to a line, then recovers
# ----------------------------------------------------------------------------
def noise_block(x0, x1, cy, half, rng, step, jitter, bias=0.35):
    """Ragged-edged band, like the min/max noise envelope on the waveform lanes."""
    n = max(2, round((x1 - x0) / step))
    xs = [x0 + (x1 - x0) * i / n for i in range(n + 1)]
    top = [cy - half - jitter * (rng.next() - bias) for _ in xs]
    bot = [cy + half + jitter * (rng.next() - bias) for _ in xs]
    pts = ["%.1f,%.1f" % (x, y) for x, y in zip(xs, top)]
    pts += ["%.1f,%.1f" % (x, y) for x, y in zip(reversed(xs), reversed(bot))]
    return "M" + " L".join(pts) + "Z"


def concept_a(size, scheme=None):
    c = SCHEMES[scheme or FINAL_SCHEME]
    AMBER = c["amber"]
    defs, shape, rim = body(size, template=True)
    defs += ('<linearGradient id="pk" x1="0" y1="0" x2="0" y2="1">'
             '<stop offset="0" stop-color="%s"/><stop offset="0.5" stop-color="%s"/>'
             '<stop offset="1" stop-color="%s"/></linearGradient>') % (c["hi"], c["mid"], c["lo"])
    g = []
    if size >= 64:
        cy, half = 520, 104
        xa, xb, xc, xd = 60, 572, 722, 964            # band runs edge to edge; gap xb..xc (ruler ticks 12 and 17)
        defs += ('<filter id="glow" x="-10%" y="-80%" width="120%" height="260%">'
                 '<feGaussianBlur stdDeviation="30"/></filter>')
        for y in (cy - 196, cy + 196):                 # amplitude rails
            g.append(rect(100, y - 2, 824, 4, "#FFFFFF", op=0.08))
        for i in range(21):                            # time ruler: 30.0 px pitch, x = 212 .. 812
            x = 212 + i * 30
            tall = i % 5 == 0
            g.append(rect(x - 3, 790 - (22 if tall else 0), 6, 20 + (22 if tall else 0), "#FFFFFF", r=3,
                          op=0.34 if tall else 0.18))
        rng = Rng(0x9E3779B97F4A7C15)
        peaks = [noise_block(xa, xb, cy, half + 16, rng, 7, 52, 0.2), noise_block(xc, xd, cy, half + 16, rng, 7, 52, 0.2)]
        core = [noise_block(xa, xb, cy, half, rng, 9, 18), noise_block(xc, xd, cy, half, rng, 9, 18)]
        g.append('<g filter="url(#glow)" opacity="0.36">%s</g>'
                 % "".join('<path d="%s" fill="%s"/>' % (d, c["mid"]) for d in core))
        g.append("".join('<path d="%s" fill="%s" opacity="0.5"/>' % (d, c["peaks"]) for d in peaks))
        g.append("".join('<path d="%s" fill="url(#pk)"/>' % d for d in core))
        g.append(rect(xb - 4, cy - 8, xc - xb + 8, 16, c["line"], r=8))               # collapsed signal
        by = cy - 250                                   # amber start/end bracket
        for x in (xb, xc):                              # cursor lines down to the ruler
            g.append(rect(x - 3, by + 20, 6, 790 - by, AMBER, op=0.30))
        g.append('<path d="M%d,%d V%d H%d V%d" fill="none" stroke="%s" stroke-width="22" '
                 'stroke-linecap="round" stroke-linejoin="round"/>' % (xb, by + 60, by, xc, by + 60, AMBER))
    elif size == 32:
        u = 32
        g.append(rect(3 * u, 11 * u, 15 * u, 10 * u, "url(#pk)"))
        g.append(rect(22 * u, 11 * u, 7 * u, 10 * u, "url(#pk)"))
        g.append(rect(18 * u, 15 * u, 4 * u, 2 * u, c["line32"]))
        g.append(rect(18 * u, 6 * u, 4 * u, 1 * u, AMBER))
        g.append(rect(18 * u, 7 * u, 1 * u, 2 * u, AMBER))
        g.append(rect(21 * u, 7 * u, 1 * u, 2 * u, AMBER))
    else:
        u = 64
        g.append(rect(1 * u, 6 * u, 8 * u, 4 * u, c["flat16"]))
        g.append(rect(11 * u, 6 * u, 4 * u, 4 * u, c["flat16"]))
        g.append(rect(9 * u, 3 * u, 2 * u, 1 * u, AMBER))
    return wrap(defs, shape + '<g clip-path="url(#clip)">' + "".join(g) + "</g>" + rim,
                "Pink Noise Audio Monitor - concept A (Dropout), %d px master" % size)


# ----------------------------------------------------------------------------
# Concept B - "Flat octaves": pink noise is flat per octave; one band has fallen
# ----------------------------------------------------------------------------
def concept_b(size):
    defs, shape, rim = body(size)
    defs += ('<linearGradient id="pk" x1="0" y1="0" x2="0" y2="1">'
             '<stop offset="0" stop-color="%s"/><stop offset="0.35" stop-color="%s"/>'
             '<stop offset="1" stop-color="%s"/></linearGradient>') % (PINK_HI, PINK, PINK_LO)
    g = []
    if size >= 64:
        bw, gap, top, bot = 104, 56, 296, 716
        x0 = 512 - (4 * bw + 3 * gap) / 2
        for k in range(5):                                   # dB graticule
            g.append(rect(196, top + k * (bot - top) / 4 - 2, 632, 4, "#FFFFFF", op=0.07))
        for i in range(4):
            x = x0 + i * (bw + gap)
            if i == 2:
                g.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="14" fill="none" stroke="%s" '
                         'stroke-width="8" stroke-dasharray="22 18" opacity="0.55"/>'
                         % (x + 4, top + 4, bw - 8, bot - top - 8, PINK))
                g.append(rect(x, bot - 124, bw, 124, WHITE, r=14))
            else:
                g.append(rect(x, top, bw, bot - top, "url(#pk)", r=14))
        g.append(rect(196, bot + 26, 632, 10, "#FFFFFF", r=5, op=0.28))
    else:
        u = 1024 / size
        if size == 32:
            cols, w, r0, r1, rd = (5, 11, 17, 23), 4, 8, 24, 18
        else:
            cols, w, r0, r1, rd = (3, 6, 9, 12), 2, 4, 12, 9
        for i, c in enumerate(cols):
            if i == 2:
                g.append(rect(c * u, rd * u, w * u, (r1 - rd) * u, WHITE))
            else:
                g.append(rect(c * u, r0 * u, w * u, (r1 - r0) * u, "url(#pk)" if size == 32 else PINK))
    return wrap(defs, shape + '<g clip-path="url(#clip)">' + "".join(g) + "</g>" + rim,
                "Pink Noise Audio Monitor - concept B (Flat octaves), %d px master" % size)


# ----------------------------------------------------------------------------
# Concept C - "Slope": the -3 dB/octave line on a log-frequency grid, pink body
# ----------------------------------------------------------------------------
def concept_c(size):
    defs, shape, rim = body(size, hi="#F792C3", lo="#D95690", rim="#FFFFFF", rim_op=0.5)
    g = []
    grid = "#4A1230"
    if size >= 64:
        px0, px1, py0, py1 = 190, 834, 250, 774
        dec = (px1 - px0) / 3
        for d in range(3):
            for k in range(1, 10):
                x = px0 + dec * (d + math.log10(k))
                g.append(rect(x - (4 if k == 1 else 2.5), py0, 8 if k == 1 else 5, py1 - py0, grid,
                              op=0.34 if k == 1 else 0.17))
        g.append(rect(px1 - 4, py0, 8, py1 - py0, grid, op=0.34))
        for k in range(5):
            y = py0 + k * (py1 - py0) / 4
            g.append(rect(px0, y - 2.5, px1 - px0, 5, grid, op=0.20))
        g.append('<path d="M%d,%d L%d,%d" stroke="%s" stroke-width="66" stroke-linecap="round"/>'
                 % (px0 + 40, py0 + 78, px1 - 40, py1 - 78, INK))
        g.append('<circle cx="%d" cy="%d" r="22" fill="%s"/>' % (px0 + 40, py0 + 78, "#FBD3E6"))
    elif size == 32:
        u = 32
        for c in (6, 12, 19, 25):
            g.append(rect(c * u, 7 * u, 1 * u, 18 * u, grid, op=0.30))
        g.append('<path d="M%d,%d L%d,%d" stroke="%s" stroke-width="%d" stroke-linecap="round"/>'
                 % (8 * u, 10 * u, 24 * u, 22 * u, INK, 3.4 * u))
    else:
        u = 64
        g.append('<path d="M%d,%d L%d,%d" stroke="%s" stroke-width="%d" stroke-linecap="round"/>'
                 % (4.5 * u, 5 * u, 11.5 * u, 11 * u, INK, 2.2 * u))
    return wrap(defs, shape + '<g clip-path="url(#clip)">' + "".join(g) + "</g>" + rim,
                "Pink Noise Audio Monitor - concept C (Slope), %d px master" % size)


CONCEPTS = {"A": concept_a, "B": concept_b, "C": concept_c,
            "A_pink": lambda size: concept_a(size, "pink"),
            "blue1": lambda size: concept_a(size, "blue1"),
            "blue2": lambda size: concept_a(size, "blue2"),
            "blue3": lambda size: concept_a(size, "blue3")}


def master_name(letter, size):
    if size >= 64: return "concept_%s.svg" % letter
    return "concept_%s_small%d.svg" % (letter, size)


LAYOUT = {1024: (0, 0), 512: (1040, 0), 256: (1040, 528), 128: (1312, 528), 64: (1312, 672), 32: (1312, 752), 16: (1312, 800)}


def render_all(letter):
    """One Chrome screenshot of a page holding every size as a fixed-size <img>
    (each SVG is rasterised by Chrome at exactly that size), then cropped apart.
    Driving Chrome with a tiny window per size proved unreliable (128/256 px
    came back clipped), so sizes are never taken from the window."""
    sys.path.insert(0, HERE)
    from pngutil import read_png, write_png, crop
    d = os.path.join(OUT, letter)
    imgs = "".join('<img src="%s" width="%d" height="%d" style="position:absolute;left:%dpx;top:%dpx">'
                   % (master_name(letter, s), s, s, x, y) for s, (x, y) in LAYOUT.items())
    page = os.path.join(d, "_render.html"); sheet = os.path.join(d, "_render.png")
    open(page, "w").write('<!doctype html><body style="margin:0">%s</body>' % imgs)
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                    "--force-device-scale-factor=1", "--default-background-color=00000000",
                    "--window-size=1568,1040", "--screenshot=" + sheet, "file://" + page],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    w, h, rows = read_png(sheet)
    assert (w, h) == (1568, 1040), (w, h)
    for s, (x, y) in LAYOUT.items():
        write_png(os.path.join(d, "%s_%d.png" % (letter, s)), s, s, crop(rows, x, y, s, s))
    os.remove(page); os.remove(sheet)


def main(which):
    for letter in which:
        d = os.path.join(OUT, letter); os.makedirs(d, exist_ok=True)
        for size in (1024, 32, 16):
            open(os.path.join(d, master_name(letter, size)), "w").write(CONCEPTS[letter](size))
        render_all(letter)
        print("rendered", letter)


if __name__ == "__main__":
    # Default: the three blue candidates and the final icon A. B, C and the pink A are
    # first-round comparison pieces; pass their names explicitly to regenerate them.
    main(sys.argv[1:] or ["blue1", "blue2", "blue3", "A"])
