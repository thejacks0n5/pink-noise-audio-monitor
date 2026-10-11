#!/usr/bin/env python3
"""Builds icon_contact_sheet.png (deliverable) and extras/zoom_small.png (an
enlarged, nearest-neighbour view of the 16 and 32 px renders for inspection).
Both are HTML pages that reference the already-rendered PNGs at 1:1 and are
screenshotted with headless Chrome."""
import os, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
NAMES = {"A": "Dropout", "B": "Flat octaves", "C": "Slope"}
FONT = "-apple-system, 'Helvetica Neue', Helvetica, Arial, sans-serif"


def shot(html_path, png_path, w, h):
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                    "--force-device-scale-factor=1", "--window-size=%d,%d" % (w, h),
                    "--screenshot=" + png_path, "file://" + html_path],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def contact_sheet(recommended="A"):
    sizes = [256, 64, 32, 16]
    cells = []
    for bg, fg, label in (("#101114", "#E8EAEE", "dark background #101114"), ("#ECECEE", "#15171B", "light background #ECECEE")):
        rows = ""
        for L in "ABC":
            imgs = "".join('<div class="c"><img src="icons/%s/%s_%d.png" width="%d" height="%d"><span>%d</span></div>'
                           % (L, L, s, s, s, s) for s in sizes)
            star = " &middot; recommended" if L == recommended else ""
            rows += '<div class="row"><div class="lab"><b>%s</b><i>%s%s</i></div>%s</div>' % (L, NAMES[L], star, imgs)
        cells.append('<div class="half" style="background:%s;color:%s"><h2>%s</h2>%s</div>' % (bg, fg, label, rows))
    html = """<!doctype html><meta charset="utf-8"><style>
body{margin:0;font-family:%s;display:flex;width:1360px}
.half{width:680px;box-sizing:border-box;padding:22px 28px 26px}
h2{font-size:12px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;opacity:.6;margin:0 0 10px}
.row{display:flex;align-items:center;height:284px}
.lab{width:96px}.lab b{display:block;font-size:44px;font-weight:700;line-height:1}
.lab i{display:block;font-style:normal;font-size:12px;opacity:.7;margin-top:6px;line-height:1.3}
.c{display:flex;flex-direction:column;align-items:center;justify-content:flex-end;height:284px;margin-right:26px}
.c img{display:block;image-rendering:pixelated}
.c span{font-size:11px;opacity:.6;margin-top:8px;font-variant-numeric:tabular-nums}
</style><body>%s</body>""" % (FONT, "".join(cells))
    p = os.path.join(ROOT, "extras", "contact_sheet.html")
    # extras/ sits beside icons/: use a relative path up one level
    html = html.replace('src="icons/', 'src="../icons/')
    open(p, "w").write(html)
    shot(p, os.path.join(ROOT, "icon_contact_sheet.png"), 1360, 22 + 22 + 3 * 284 + 26)


def zoom_sheet():
    blocks = ""
    for bg in ("#101114", "#ECECEE", "#3A3D46"):
        for L in "ABC":
            for s, z in ((16, 12), (32, 6)):
                blocks += ('<div style="background:%s;padding:14px;display:inline-block">'
                           '<img src="../icons/%s/%s_%d.png" width="%d" height="%d" style="image-rendering:pixelated;display:block"></div>'
                           % (bg, L, L, s, s * z, s * z))
        blocks += "<br>"
    html = '<!doctype html><body style="margin:0;background:#666;line-height:0">%s</body>' % blocks
    p = os.path.join(ROOT, "extras", "zoom_small.html")
    open(p, "w").write(html)
    shot(p, os.path.join(ROOT, "extras", "zoom_small.png"), 6 * (192 + 28), 3 * (192 + 28))


if __name__ == "__main__":
    import sys
    contact_sheet(sys.argv[1] if len(sys.argv) > 1 else "A")
    zoom_sheet()
