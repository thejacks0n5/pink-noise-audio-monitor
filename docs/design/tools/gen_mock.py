#!/usr/bin/env python3
"""Draws theme_preview.svg (a 1120x720 mock of the main window) from the
palette in palette.py and renders theme_preview.png with headless Chrome.

Geometry follows MainComponent::resized, TopStatusBar, TransportControls,
LevelMeterComponent, WaveformDisplay and EventLogComponent as read from the
repo; it is a drawing, not a screenshot. `python3 gen_mock.py current` draws
the same window with the palette that ships today, for comparison."""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from palette import NEW, CUR, rgb, alpha, css, over, rot_hue, tohex, juce_overlaid
from pngutil import read_png, write_png, crop

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
W, H = 1120, 720
SANS = "'Lucida Grande', 'Helvetica Neue', Helvetica, Arial, sans-serif"   # JUCE's default sans on macOS
MONO = "Menlo, Monaco, monospace"                                            # JUCE's default monospace on macOS
K = 0.85   # JUCE font "height" is ascent+descent; em size is about 0.85 of it for these faces


class Mock:
    def __init__(self, pal):
        self.p = pal
        self.o = []

    def c(self, name): return css(self.p[name])

    def rect(self, x, y, w, h, fill, r=0, stroke=None, op=None):
        s = '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s"' % (x, y, w, h, fill)
        if r: s += ' rx="%g"' % r
        if stroke: s += ' stroke="%s" stroke-width="1"' % stroke
        if op is not None: s += ' opacity="%g"' % op
        self.o.append(s + "/>")

    def outlined(self, x, y, w, h, fill, stroke, r=4):
        """JUCE idiom: bounds.reduced(0.5), fillRoundedRectangle + 1 px outline."""
        self.rect(x + 0.5, y + 0.5, w - 1, h - 1, fill, r=r, stroke=stroke)

    def hline(self, y, x0, x1, col): self.rect(x0, y, x1 - x0, 1, col)
    def vline(self, x, y0, y1, col): self.rect(x, y0, 1, y1 - y0, col)

    def text(self, x, y, s, size, fill, anchor="start", mono=False, op=None, rotate=None, weight=None, fit=None):
        a = ' text-anchor="%s"' % anchor if anchor != "start" else ""
        est = len(s) * size * K * (0.602 if mono else 0.56)
        if fit and est > fit:        # juce::Graphics::drawFittedText squeezes glyphs horizontally
            a += ' textLength="%.1f" lengthAdjust="spacingAndGlyphs"' % fit
        a += ' opacity="%g"' % op if op is not None else ""
        a += ' transform="rotate(%s)"' % rotate if rotate else ""
        a += ' font-weight="%s"' % weight if weight else ""
        s = s.replace("&", "&amp;").replace("<", "&lt;")
        self.o.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="%.2f" fill="%s" dominant-baseline="central"%s>%s</text>'
                      % (x, y, MONO if mono else SANS, size * K, fill, a, s))

    # ---- widgets -------------------------------------------------------
    def button(self, x, y, w, h, label, state="normal"):
        fill = self.c("controlBackground"); txt = self.c("textPrimary"); edge = self.c("outline")
        if state == "on": fill, txt = self.c("accentDim"), self.c("textOnAccent")
        if state == "disabled": txt = self.c("textDisabled")
        if state == "focus": edge = self.c("outlineFocused")
        if state == "hover": fill = self.c("controlHover")
        if state == "down": fill = self.c("controlPressed")
        self.outlined(x, y, w, h, fill, edge)
        self.text(x + w / 2, y + h / 2, label, min(16.0, h * 0.5), txt, anchor="middle")

    def combo(self, x, y, w, h, label):
        self.outlined(x, y, w, h, self.c("controlBackground"), self.c("outline"))
        self.text(x + 8, y + h / 2, label, min(16.0, h * 0.55), self.c("textPrimary"))
        cx, cy = x + w - h / 2 - 2, y + h / 2
        self.o.append('<path d="M%.1f,%.1f L%.1f,%.1f L%.1f,%.1f Z" fill="%s"/>'
                      % (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3, self.c("textSecondary")))

    # ---- sections ------------------------------------------------------
    def status_bar(self, x, y, w, h):
        self.rect(x, y, w, h, self.c("panelBackground"))
        # Cells and preferred widths as TopStatusBar::resized lays them out in the working tree
        # on 2026-10-10 (no MODE cell; LOG widened to 130; 230 px reserved for the app-status text).
        spec = [("AUDIO", "Running", 105, "statusRunning", False), ("LOG", "Logging", 130, "statusRunning", False),
                ("SESSION", "38:14:07", 95, None, False),
                ("OUT", "UMC404HD 192k", 165, None, True), ("IN", "UMC404HD 192k", 175, None, True),
                ("OUT CH", "2 ch (stereo)", 120, None, True), ("IN CH", "4 ch", 120, None, True),
                ("RATE", "48.0 kHz", 100, None, True), ("BUFFER", "512 smp (10.7 ms)", 155, None, True)]
        scale = min(1.0, (w - 230) / sum(s[2] for s in spec))
        cx = x
        for cap, val, pw, lamp, interactive in spec:
            cw = round(pw * scale)
            ax, ay, aw = cx + 6, y + 4, cw - 12
            if interactive:
                aw -= 12
                qx, qy = ax + aw + 6, y + h / 2 + 3
                self.o.append('<path d="M%.1f,%.1f L%.1f,%.1f L%.1f,%.1f" fill="none" stroke="%s" stroke-width="1.2"/>'
                              % (qx - 3.5, qy - 2.1, qx, qy + 2.1, qx + 3.5, qy - 2.1, self.c("textSecondary")))
            tx = ax
            if lamp:
                self.o.append('<circle cx="%.1f" cy="%.1f" r="3.5" fill="%s"/>' % (ax + 3.5, ay + 6, self.c(lamp)))
                tx += 11
            self.text(tx, ay + 6, cap, 9.5, self.c("textSecondary"))
            self.text(ax, ay + 12 + (h - 8 - 12) / 2, val, 13, self.c("textPrimary"), fit=aw)
            self.vline(cx + cw - 1, y + 6, y + h - 6, self.c("outline"))
            cx += cw
        self.text(cx + 6, y + 4 + 12 + (h - 20) / 2, "Logging", 13, self.c("textPrimary"))

    def transport(self, x, y, w, h):
        self.rect(x, y, w, h, self.c("panelBackground"))
        self.hline(y, x, x + w, self.c("outline")); self.hline(y + h - 1, x, x + w, self.c("outline"))
        bx, by, bh = x + 8, y + 6, h - 12
        right = x + w - 8
        for label, bw, gap, st in (("Audio Setup", 110, 12, "normal"), ("Export", 70, 4, "normal"),
                                   ("Reset", 70, 4, "disabled"), ("Marker", 70, 12, "normal")):
            right -= bw
            self.button(right, by, bw, bh, label, st)
            right -= gap
        cur = bx
        for label, bw, gap, st in (("Start Audio", 90, 4, "disabled"), ("Stop Audio", 90, 12, "normal"),
                                   ("Start Logging", 100, 4, "disabled"), ("Pause", 70, 4, "normal"),
                                   ("Stop Logging", 100, 12, "normal"), ("Mute", 70, 4, "normal")):
            self.button(cur, by, bw, bh, label, st)
            cur += bw + gap
        self.text(cur, by + bh / 2, "Output", 12, self.c("textPrimary"))
        cur += 46
        sw = min(240, right - cur)
        # slider: track + accent fill + thumb, then the dBFS text box on the right
        tw = sw - 64
        ty = by + bh / 2
        pos = cur + 6 + (tw - 12) * (40.0 / 60.0)          # -20 dBFS on a -60..0 scale
        self.rect(cur + 6, ty - 2, tw - 12, 4, self.c("controlBackground"), r=2)
        self.rect(cur + 6, ty - 2, pos - cur - 6, 4, self.c("accent"), r=2)
        self.o.append('<circle cx="%.1f" cy="%.1f" r="6" fill="%s" stroke="%s" stroke-width="1"/>'
                      % (pos, ty, self.c("accentBright"), self.c("outline")))
        self.rect(cur + tw + 4.5, ty - 9.5, 59, 19, self.c("controlBackground"), stroke=self.c("outline"))
        self.text(cur + tw + 34, ty, "-20.0 dBFS", 14, self.c("textPrimary"), anchor="middle", mono=True, fit=54)

    def meter(self, x, y, w, h, title, names, peak, rms, hold, clip):
        p = self.p
        self.rect(x, y, w, h, self.c("panelBackground"))
        self.text(x + w / 2, y + 8, title, 13, self.c("textSecondary"), anchor="middle")
        n = len(names)
        bar_y, bar_h = y + 26, h - 26 - 14 - 48
        bw = max(2, (w - 4 * (n - 1) - 26) // n)
        prop = lambda db: min(1.0, max(0.0, (db + 60.0) / 60.0))
        for i in range(n):
            bx = x + i * (bw + 4)
            lamp_x = bx + (bw - 8) / 2
            self.o.append('<circle cx="%.1f" cy="%.1f" r="4" fill="%s" stroke="%s" stroke-width="1"/>'
                          % (lamp_x + 4, y + 20, self.c("meterRed") if clip[i] else self.c("meterTrough"), self.c("outline")))
            self.rect(bx, bar_y, bw, bar_h, self.c("meterTrough"))
            lit = round(prop(peak[i]) * bar_h)
            if lit > 0:
                zone = "meterRed" if peak[i] >= -3 else ("meterAmber" if peak[i] >= -12 else "meterGreen")
                self.rect(bx, bar_y + bar_h - lit, bw, lit, self.c(zone))
            # rotated channel caption, reading upward from under the bar
            sx, sy = bx + bw / 2, bar_y + bar_h + 24
            self.text(sx + 22, sy, names[i], 9.5, self.c("textSecondary"), anchor="end",
                      rotate="-90 %.1f %.1f" % (sx, sy))
            rh = round(prop(rms[i]) * bar_h)
            if rh > 0:
                self.rect(bx, bar_y + bar_h - rh, bw, rh, css(p["meterRmsOverlay"]))
            if hold[i] > -100:
                hy = bar_y + bar_h - round(prop(hold[i]) * bar_h)
                self.rect(bx, min(max(bar_y, hy - 1), bar_y + bar_h - 2), bw, 2, self.c("meterPeakTick"))
            self.rect(bx + 0.5, bar_y + 0.5, bw - 1, bar_h - 1, "none", stroke=self.c("outline"))
        rx = x + w - 24
        for tick in (0, -3, -6, -12, -20, -30, -40, -50, -60):
            ty = bar_y + bar_h - round(prop(tick) * bar_h)
            self.rect(rx, ty, 4, 1, self.c("textSecondary"))
            self.text(rx + 6, ty, str(tick), 9, self.c("textSecondary"))
        txt = "  ".join(("%.1f" % v) if v > -100 else "-inf" for v in peak)
        self.text(x + w / 2, y + h - 7, txt, 12, self.c("textPrimary"), anchor="middle", mono=True, fit=w - 6)

    def waveform(self, x, y, w, h, lanes):
        self.rect(x, y, w, h, self.c("windowBackground"))
        n = len(lanes)
        lh = h // n
        seed = [0x2545F4914F6CDD1D]
        def rnd():
            s = seed[0]; s ^= (s >> 12); s ^= (s << 25) & 0xFFFFFFFFFFFFFFFF; s ^= (s >> 27); seed[0] = s
            return ((s * 0x2545F4914F6CDD1D) & 0xFFFFFFFFFFFFFFFF) / 2 ** 64
        for i, (label, fam, ch, env) in enumerate(lanes):
            ly = y + i * lh
            this_h = lh if i < n - 1 else h - lh * (n - 1)
            ty0, th = ly + 24, this_h - 24
            cy = ty0 + th // 2
            if i > 0: self.hline(ly, x, x + w, self.c("outline"))
            self.hline(cy, x, x + w, self.c("traceCentreLine"))
            col = tohex(rot_hue(rgb(self.p[fam]), 0.045 * ch))
            half = th * 0.5 - 2.0
            top, bot = [], []
            for px_ in range(w):
                a = env(px_ / w)
                mx = a * (0.62 + 0.38 * rnd()); mn = -a * (0.62 + 0.38 * rnd())
                top.append("%d,%.1f" % (x + px_, cy - mx * half)); bot.append("%d,%.1f" % (x + px_, cy - mn * half))
            for pts in (top, bot):
                self.o.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="1"/>' % (" ".join(pts), col))
            self.combo(x + 6, ly + 3, 150, 18, label)
        self.combo(x + w - 96, y + 3, 90, 18, "%d lanes" % n)

    def event_log(self, x, y, w, h, rows):
        self.rect(x, y, w, h, self.c("panelBackground"))
        tx, ty = x + 4, y + 2
        self.combo(tx, ty, 120, 20, "All severities"); tx += 124
        self.combo(tx, ty, 140, 20, "All types"); tx += 144
        # Follow toggle (ticked)
        self.rect(tx, ty + 2, 16, 16, self.c("controlBackground"), r=3, stroke=self.c("outline"))
        self.rect(tx + 3, ty + 5, 10, 10, self.c("accent"), r=2)
        self.text(tx + 24, ty + 10, "Follow", 14, self.c("textPrimary")); tx += 74
        self.button(tx, ty, 60, 20, "Clear"); tx += 64
        self.text(tx + 6, ty + 10, "%d / %d events" % (len(rows), len(rows)), 13, self.c("textPrimary"))
        cols = [("Time", 110, "l", True), ("Elapsed", 70, "r", True), ("Type", 130, "l", False), ("Severity", 80, "l", False),
                ("Ch", 40, "c", False), ("Measured", 80, "r", True), ("Expected", 80, "r", True), ("Diff", 70, "r", True),
                ("Description", 280, "l", False), ("Note", 160, "l", False)]
        hy = y + 24
        self.rect(x, hy, w, 22, self.c("panelBackgroundAlt"))
        cx = x
        for name, cw, _, _ in cols:
            self.text(cx + 8, hy + 11, name, 13, self.c("textSecondary"))
            self.vline(cx + cw - 1, hy, hy + 22, self.c("outline"))
            cx += cw
        self.hline(hy + 21, x, x + w, self.c("outline"))
        ry = hy + 22
        sev_col = {"Info": "severityInfo", "Warning": "severityWarning", "Critical": "severityCritical"}
        for i, row in enumerate(rows):
            if ry + 20 > y + h: break
            base = rgb(self.p["panelBackground" if i % 2 == 0 else "panelBackgroundAlt"])
            if row[3] == "Critical":
                base = juce_overlaid(base, rgb(self.p["severityCritical"]), 0.10)
            if len(row) > 10 and row[10] == "selected":
                base = rgb(self.p["controlHover"])
            self.rect(x, ry, w, 20, tohex(base))
            self.rect(x, ry, 3, 20, self.c(sev_col[row[3]]))
            cx = x
            for j, (name, cw, just, mono) in enumerate(cols):
                colr = self.c(sev_col[row[3]]) if name == "Severity" else self.c("textPrimary")
                lp = 6 if j == 0 else 4
                if just == "l": self.text(cx + lp, ry + 10, row[j], 12, colr, mono=mono)
                elif just == "r": self.text(cx + cw - 4, ry + 10, row[j], 12, colr, anchor="end", mono=mono)
                else: self.text(cx + cw / 2, ry + 10, row[j], 12, colr, anchor="middle", mono=mono)
                cx += cw
            ry += 20

    def svg(self):
        return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
               '>\n%s\n</svg>\n' % (W, H, W, H, "\n".join(self.o)))


def build(pal):
    m = Mock(pal)
    m.rect(0, 0, W, H, m.c("windowBackground"))
    m.status_bar(8, 8, W - 16, 44)
    m.transport(8, 60, W - 16, 56)
    top, bottom = 124, H - 8
    log_h = min(280, max(140, round((bottom - top) * 0.30)))
    log_y = bottom - log_h
    area_h = log_y - 8 - top
    outs, ins = 2, 4
    meter_w = min(max(200, (W - 16) // 2), max(200, 60 + 26 * (outs + ins)))
    ref_w = max(60, round((meter_w - 4) * outs / (outs + ins)))
    m.meter(8, top, ref_w, area_h, "REF OUT", ["FOH L", "FOH R"], [-14.2, -14.6], [-20.0, -20.1], [-12.9, -13.3], [False, False])
    m.meter(8 + ref_w + 4, top, meter_w - ref_w - 4, area_h, "INPUT", ["Rtn L", "Rtn R", "Amp", "Sub"],
            [-15.1, -200.0, -9.8, -1.2], [-21.0, -200.0, -15.6, -7.4], [-13.8, -14.9, -8.9, -0.1],
            [False, False, False, True])
    wx = 8 + meter_w + 8
    steady = lambda t: 0.30
    drop = lambda t: 0.0 if 0.62 < t < 0.80 else 0.29
    step = lambda t: 0.29 if t < 0.44 else 0.52
    m.waveform(wx, top, W - 8 - wx, area_h, [("Out 1 - FOH L", "traceReference", 0, steady),
                                              ("In 1 - Rtn L", "traceInputL", 0, steady),
                                              ("In 2 - Rtn R", "traceInputL", 1, drop),
                                              ("In 3 - Amp", "traceInputL", 2, step)])
    rows = [
        ("19:02:11.640", "129731.640", "ManualMarker", "Info", "-", "0.00", "0.00", "0.00", "Manual marker", "doors"),
        ("21:14:02.118", "137642.118", "AudioSettingsChanged", "Info", "-", "256.00", "512.00", "-256.00", "Buffer size changed 512 > 256", ""),
        ("21:40:55.302", "139255.302", "LevelChange", "Warning", "Amp", "-9.82", "-15.10", "5.28", "Level change on Amp: +5.3 dB (start)", ""),
        ("21:41:07.871", "139267.871", "CompleteDropout", "Critical", "Rtn R", "-96.00", "-15.10", "-80.90", "Input silent while reference active (start)", ""),
        ("21:41:09.455", "139269.455", "Clipping", "Critical", "Sub", "-0.02", "-15.10", "15.08", "Clipping: 37 samples at full scale", ""),
        ("21:41:12.006", "139272.006", "ManualMarker", "Info", "-", "0.00", "0.00", "0.00", "Manual marker", "amp rack check"),
        ("21:41:30.990", "139290.990", "ManualMarker", "Info", "-", "0.00", "0.00", "0.00", "Manual marker", ""),
    ]
    m.event_log(8, log_y, W - 16, log_h, rows)
    return m.svg()


def render(svg_path, png_path):
    page = svg_path + ".html"; sheet = png_path + ".tmp.png"
    open(page, "w").write('<!doctype html><body style="margin:0;background:#000"><img src="%s" width="%d" height="%d" '
                          'style="display:block"></body>' % (os.path.basename(svg_path), W, H))
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--window-size=%d,%d" % (W + 80, H + 80), "--screenshot=" + sheet, "file://" + page],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    w, h, rows = read_png(sheet)
    write_png(png_path, W, H, crop(rows, 0, 0, W, H))
    os.remove(page); os.remove(sheet)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "current":
        svg, png = os.path.join(ROOT, "extras", "theme_preview_shipping_palette.svg"), os.path.join(ROOT, "extras", "theme_preview_shipping_palette.png")
        open(svg, "w").write(build(CUR)); render(svg, png)
    else:
        svg, png = os.path.join(ROOT, "theme_preview.svg"), os.path.join(ROOT, "theme_preview.png")
        open(svg, "w").write(build(NEW)); render(svg, png)
    print("wrote", png)
