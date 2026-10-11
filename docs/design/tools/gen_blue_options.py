#!/usr/bin/env python3
"""Builds blue_options.png (icon A in the three blue candidates at 256, 64, 32,
16 px on dark and light) and extras/blue_options_zoom.png (their 16 and 32 px
renders enlarged, nearest-neighbour, for judging the small sizes)."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from gen_icons import SCHEMES, FINAL_SCHEME
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
FONT = "-apple-system, 'Helvetica Neue', Helvetica, Arial, sans-serif"
KEYS = ["blue1", "blue2", "blue3"]
DESC = {"blue1": "Electric: cold, high chroma, toward cyan", "blue2": "Steel / ice: desaturated, metallic",
        "blue3": "Honed: a truer, deeper blue"}

def shot(html, png, w, h):
    p = os.path.join(ROOT, "extras", "_sheet.html"); open(p, "w").write(html)
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--window-size=%d,%d" % (w, h), "--screenshot=" + png, "file://" + p],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.remove(p)

def options():
    halves = ""
    for bg, fg, label in (("#101114", "#E8EAEE", "dark background #101114"), ("#ECECEE", "#15171B", "light background #ECECEE")):
        rows = ""
        for i, k in enumerate(KEYS):
            c = SCHEMES[k]
            imgs = "".join('<div class="c"><img src="../icons/%s/%s_%d.png" width="%d" height="%d"><span>%d</span></div>'
                           % (k, k, s, s, s, s) for s in (256, 64, 32, 16))
            rec = '<u>recommended</u>' if k == FINAL_SCHEME else ""
            rows += ('<div class="row"><div class="lab"><b>%d</b><i>%s</i><em><span class="sw" style="background:%s"></span>band %s</em>%s</div>%s</div>'
                     % (i + 1, DESC[k], c["mid"], c["mid"].upper(), rec, imgs))
        halves += '<div class="half" style="background:%s;color:%s"><h2>%s</h2>%s</div>' % (bg, fg, label, rows)
    html = """<!doctype html><meta charset="utf-8"><style>
body{margin:0;font-family:%s;display:flex;width:1440px}
.half{width:720px;box-sizing:border-box;padding:22px 28px 26px}
h2{font-size:12px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;opacity:.6;margin:0 0 10px}
.row{display:flex;align-items:center;height:284px}
.lab{width:150px}.lab b{display:block;font-size:44px;font-weight:700;line-height:1}
.lab i{display:block;font-style:normal;font-size:12px;opacity:.75;margin-top:6px;line-height:1.3}
.lab em{display:flex;align-items:center;font-style:normal;font:12px Menlo,monospace;margin-top:8px}
.lab u{display:inline-block;text-decoration:none;font-size:11px;font-weight:600;border:1px solid currentColor;border-radius:3px;padding:1px 5px;margin-top:8px}
.sw{display:inline-block;width:12px;height:12px;margin-right:6px;border:1px solid rgba(128,128,128,.5)}
.c{display:flex;flex-direction:column;align-items:center;justify-content:flex-end;height:284px;margin-right:24px}
.c img{display:block;image-rendering:pixelated}
.c span{font-size:11px;opacity:.6;margin-top:8px}
</style><body>%s</body>""" % (FONT, halves)
    shot(html, os.path.join(ROOT, "blue_options.png"), 1440, 22 + 22 + 3 * 284 + 26)

def zoom():
    blocks = ""
    for bg in ("#101114", "#ECECEE", "#3A3D46"):
        for k in KEYS:
            for s, z in ((16, 12), (32, 6)):
                blocks += ('<div style="background:%s;padding:14px;display:inline-block">'
                           '<img src="../icons/%s/%s_%d.png" width="%d" height="%d" style="image-rendering:pixelated;display:block"></div>'
                           % (bg, k, k, s, s * z, s * z))
        blocks += "<br>"
    shot('<!doctype html><body style="margin:0;background:#666;line-height:0">%s</body>' % blocks,
         os.path.join(ROOT, "extras", "blue_options_zoom.png"), 6 * (192 + 28), 3 * (192 + 28))

if __name__ == "__main__":
    options(); zoom()
