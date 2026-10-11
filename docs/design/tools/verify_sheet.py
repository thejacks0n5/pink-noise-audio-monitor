#!/usr/bin/env python3
"""Lays the system renders from verify/system_renders/ out for inspection:
verify/system_small_zoom.png (16, 32, 64 px enlarged, system render beside the
file that went into the catalog) and verify/system_all_sizes.png (every size at
1:1, 1024 shown at half size), each on a dark and a light background. Also
prints, per size, where the coloured band sits, which is how a plate shows up."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from pngutil import read_png
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
SIZES = [16, 32, 64, 128, 256, 512, 1024]

def shot(html, png, w, h):
    p = os.path.join(ROOT, "verify", "_sheet.html"); open(p, "w").write(html)
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--window-size=%d,%d" % (w, h), "--screenshot=" + png, "file://" + p],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.remove(p)

def band(path):
    """Fraction of the canvas width the coloured band spans on its widest row (any saturated,
    non-amber pixels; the body is near-neutral), plus amber pixel count."""
    w, h, rows = read_png(path)
    best = (0, 0); amber = 0
    for y in range(h):
        r = rows[y]; xs = []
        for x in range(w):
            R, G, B, A = r[x * 4:x * 4 + 4]
            amberish = R > 200 and G > 160 and B < 130
            if A > 200 and max(R, G, B) - min(R, G, B) > 90 and max(R, G, B) > 150 and not amberish: xs.append(x)
            if A > 200 and R > 200 and G > 160 and B < 130: amber += 1
        if xs and xs[-1] - xs[0] > best[1] - best[0]: best = (xs[0], xs[-1])
    opaque = [x for x in range(w) if rows[h // 2][x * 4 + 3] > 250]
    return best[0] / w, (best[1] + 1) / w, amber, (opaque[0], opaque[-1]) if opaque else None

if __name__ == "__main__":
    S = "system_renders/system_%d.png"
    for s in SIZES:
        lo, hi, amber, op = band(os.path.join(ROOT, "verify", S % s))
        ok = lo < 0.14 and hi > 0.86
        print("%5d px  band %.3f..%.3f of width  %s  amber px %d  opaque x %s" % (
            s, lo, hi, "FULL-BLEED" if ok else "SHRUNK / PLATE", amber, op))
    if os.path.isdir(os.path.join(ROOT, "verify", "icns_only_renders")):
        print("control, fallback AppIcon.icns alone:")
        for s in SIZES:
            lo, hi, amber, op = band(os.path.join(ROOT, "verify", "icns_only_renders", "system_%d.png" % s))
            print("%5d px  band %.3f..%.3f of width  %s  amber px %d" % (
                s, lo, hi, "FULL-BLEED" if lo < 0.14 and hi > 0.86 else "SHRUNK / PLATE", amber))
    cell = '<div class="c"><img src="%s" width="%d" height="%d"><span>%s</span></div>'
    zoom = ""
    for bg, fg in (("#101114", "#E8EAEE"), ("#ECECEE", "#15171B")):
        row = ""
        for s, z in ((16, 14), (32, 7), (64, 4)):
            row += cell % (S % s, s * z, s * z, "macOS draws, %d px (x%d)" % (s, z))
            row += cell % ("../icons/A/A_%d.png" % s, s * z, s * z, "file in catalog, %d px" % s)
        zoom += '<div class="band" style="background:%s;color:%s">%s</div>' % (bg, fg, row)
    css = ("<style>body{margin:0;font:11px -apple-system,Helvetica,sans-serif}.band{display:flex;gap:18px;padding:18px;align-items:flex-end}"
           ".c{display:flex;flex-direction:column;align-items:center}.c img{image-rendering:pixelated;display:block}.c span{margin-top:6px;opacity:.7}</style>")
    shot("<!doctype html>" + css + "<body>" + zoom + "</body>", os.path.join(ROOT, "verify", "system_small_zoom.png"), 6 * 242 + 20, 2 * 300)
    allsz = ""
    for bg, fg in (("#101114", "#E8EAEE"), ("#ECECEE", "#15171B")):
        row = cell % (S % 1024, 512, 512, "1024 px (shown at half size)")
        for s in (512, 256, 128, 64, 32, 16):
            row += cell % (S % s, s, s, "%d" % s)
        allsz += '<div class="band" style="background:%s;color:%s">%s</div>' % (bg, fg, row)
    shot("<!doctype html>" + css + "<body>" + allsz + "</body>", os.path.join(ROOT, "verify", "system_all_sizes.png"), 512 * 2 + 256 + 128 + 64 + 32 + 16 + 8 * 18 + 20, 2 * 566)
