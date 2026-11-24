"""Draws the README's animated SVGs into docs/img/: hero.svg (music story), activities.svg (rest-line status slot) and
cards.svg (takeovers). Sizes, colours and the spring come from island/src/tokens.h. Text is turned into paths from the
bundled Manrope, so the files need no fonts and look the same everywhere. Animation is SMIL only (no script), which GitHub
plays inside an <img>. Run: python make_readme_art.py   (needs: pip install fonttools)"""
import math, os
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "docs", "img")

# tokens.h
INK, INK2_A, TRACK_A, BTN_A = "#F5F6F8", 0.62, 0.20, 0.14
ACCENT, ON_ACCENT = "#4CC2FF", "#101114"
GLASS = "#1C1E24"
COLLAPSED_H, COLLAPSED_R, EXPANDED_W, EXPANDED_H, EXPANDED_R, CARD_H = 34, 11, 368, 168, 22, 104
PAD_X, PAD_Y, TOP = 18, 16, 14
STIFF, DAMP = 420.0, 34.0
FPS = 60

# ---------------------------------------------------------------- text as paths
_fonts = {}
def font(weight):
    if weight not in _fonts:
        f = TTFont(os.path.join(ROOT, "island", "assets", "Manrope.ttf"))
        _fonts[weight] = instancer.instantiateVariableFont(f, {"wght": weight})
    return _fonts[weight]

def text_w(s, size, weight=400):
    f = font(weight); cmap = f.getBestCmap(); hm = f["hmtx"]
    return sum(hm[cmap[ord(c)]][0] for c in s) * size / f["head"].unitsPerEm

def text(s, x, y, size, weight=400, fill=INK, opacity=None, anchor="start", mid=True):
    """One <path> for the string. y is the vertical middle of the capitals when mid, else the baseline."""
    f = font(weight); cmap = f.getBestCmap(); hm = f["hmtx"]; gs = f.getGlyphSet()
    k = size / f["head"].unitsPerEm
    if anchor == "end": x -= text_w(s, size, weight)
    elif anchor == "middle": x -= text_w(s, size, weight) / 2
    base = y + f["OS/2"].sCapHeight * k / 2 if mid else y
    pen = SVGPathPen(gs, ntos=lambda v: f"{v:.2f}".rstrip("0").rstrip("."))
    cx = x
    for c in s:
        g = cmap[ord(c)]
        gs[g].draw(TransformPen(pen, (k, 0, 0, -k, cx, base)))
        cx += hm[g][0] * k
    op = f' fill-opacity="{opacity}"' if opacity is not None else ""
    return f'<path d="{pen.getCommands()}" fill="{fill}"{op}/>'

# ---------------------------------------------------------------- timeline -> SMIL
class Clip:
    """All animations of one file share one duration and loop together, so they never drift apart."""
    def __init__(self, dur): self.dur = dur

    def anim(self, attr, keys, start, extra=""):
        """keys: [(t, value)], held between keys, linear inside dense samples. Returns an <animate> element."""
        pts = [(0.0, start)] + [(t, v) for t, v in keys] + [(self.dur, keys[-1][1] if keys else start)]
        times, vals = [], []
        for t, v in pts:
            t = min(max(t, 0.0), self.dur)
            if times and t <= times[-1] + 1e-6: t = times[-1] + 1e-4
            times.append(t); vals.append(v)
        kt = ";".join(f"{t / self.dur:.5f}".rstrip("0").rstrip(".") or "0" for t in times)
        kt = kt.split(";"); kt[0] = "0"; kt[-1] = "1"
        vs = ";".join(_num(v) for v in vals)
        return (f'<animate attributeName="{attr}" dur="{self.dur}s" repeatCount="indefinite" calcMode="linear" '
                f'keyTimes="{";".join(kt)}" values="{vs}"{extra}/>')

def _num(v):
    if isinstance(v, str): return v
    return f"{v:.2f}".rstrip("0").rstrip(".") if abs(v - round(v)) > 1e-3 else str(int(round(v)))

def spring_curve():
    """Progress 0..1 of the island's spring (mass 1), sampled at FPS until it settles."""
    x, v, out, dt = 0.0, 0.0, [], 1.0 / FPS
    for i in range(int(0.8 * FPS)):
        out.append(x)
        for _ in range(8):  # substeps keep the integration stable
            a = -STIFF * (x - 1) - DAMP * v
            v += a * dt / 8; x += v * dt / 8
    out.append(1.0)
    return out
SPRING = spring_curve()

def morph(events):
    """events: [(t, w, h, r)] -> per-attribute key lists for the pill, each change sprung like the real island."""
    w0, h0, r0 = events[0][1:]
    ks = {"w": [], "h": [], "r": []}
    cur = (w0, h0, r0)
    for i, (t, w, h, r) in enumerate(events[1:], 1):
        nxt = events[i + 1][0] if i + 1 < len(events) else 1e9
        for j, p in enumerate(SPRING):
            tt = t + j / FPS
            if tt >= nxt: break
            for name, a, b in (("w", cur[0], w), ("h", cur[1], h), ("r", cur[2], r)):
                ks[name].append((tt, a + (b - a) * p))
        cur = (w, h, r)
    return (w0, h0, r0), ks

def fades(shows):
    """shows: [(t_in, t_out)] -> opacity keys: out takes 110 ms, in 140 ms after a short wait (tokens contentOut/InMs)."""
    keys = []
    for a, b in shows:
        if a > 0: keys += [(a + 0.06, 0), (a + 0.2, 1)]
        if b is not None: keys += [(b, 1), (b + 0.11, 0)]
    return keys

# ---------------------------------------------------------------- pieces
def rrect(x, y, w, h, r, fill, opacity=None, extra=""):
    op = f' fill-opacity="{opacity}"' if opacity is not None else ""
    return f'<rect x="{_num(x)}" y="{_num(y)}" width="{_num(w)}" height="{_num(h)}" rx="{_num(r)}" fill="{fill}"{op}{extra}/>'

def usage_bars(x, cy, cpu, ram, clip=None, cpu_keys=None):
    """The rest line's two 26x4 bars: CPU on top, RAM below."""
    s = []
    for i, (val, keys) in enumerate(((cpu, cpu_keys), (ram, None))):
        y = cy - 5 if i == 0 else cy + 1
        s.append(rrect(x, y, 26, 4, 2, INK, TRACK_A))
        fill = rrect(x, y, 26 * val, 4, 2, ACCENT)
        if keys and clip:
            fill = fill[:-2] + ">" + clip.anim("width", [(t, 26 * v) for t, v in keys], 26 * val) + "</rect>"
        s.append(fill)
    return "".join(s)

def eq(x, cy, h=14):
    """Four equalizer bars bouncing on their own short loop (eqLoopMs 900)."""
    s = []
    seqs = [(0.45, 1, 0.6, 0.85), (1, 0.5, 0.9, 0.4), (0.6, 0.95, 0.35, 0.8), (0.3, 0.7, 1, 0.5)]
    for i, seq in enumerate(seqs):
        bx = x + i * 5
        vals = list(seq) + [seq[0]]
        hs = ";".join(_num(h * v) for v in vals); ys = ";".join(_num(cy - h * v / 2) for v in vals)
        a = f'dur="0.9s" repeatCount="indefinite" begin="{-i * 0.17:.2f}s" calcMode="spline" keySplines="{";".join(["0.4 0 0.6 1"] * 4)}"'
        s.append(f'<rect x="{bx}" y="{_num(cy - h * seq[0] / 2)}" width="3" height="{_num(h * seq[0])}" rx="1.5" fill="{ACCENT}">'
                 f'<animate attributeName="height" values="{hs}" {a}/><animate attributeName="y" values="{ys}" {a}/></rect>')
    return "".join(s)
EQ_W = 4 * 3 + 3 * 2

def art(x, y, size, r, gid):
    return (f'<rect x="{_num(x)}" y="{_num(y)}" width="{size}" height="{size}" rx="{r}" fill="url(#{gid})"/>'
            f'<circle cx="{_num(x + size * 0.68)}" cy="{_num(y + size * 0.36)}" r="{_num(size * 0.16)}" fill="#FFE2B8" fill-opacity="0.9"/>'
            f'<path d="M{_num(x)} {_num(y + size * 0.78)} Q{_num(x + size * 0.35)} {_num(y + size * 0.52)} {_num(x + size)} {_num(y + size * 0.74)} '
            f'V{_num(y + size - r)} Q{_num(x + size)} {_num(y + size)} {_num(x + size - r)} {_num(y + size)} H{_num(x + r)} '
            f'Q{_num(x)} {_num(y + size)} {_num(x)} {_num(y + size - r)}Z" fill="#2B1B3D" fill-opacity="0.55"/>')

ART_DEF = ('<linearGradient id="art" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#FF8A5B"/>'
           '<stop offset="0.55" stop-color="#E2557E"/><stop offset="1" stop-color="#6A4BC4"/></linearGradient>')

# Icons, drawn on a 16 box at (x, y) top-left, stroked in the accent (stand-ins for the Fluent glyphs the app uses).
def icon(name, x, y, color=ACCENT, sw=1.5, size=16):
    k = size / 16
    st = f'fill="none" stroke="{color}" stroke-width="{_num(sw)}" stroke-linecap="round" stroke-linejoin="round"'
    T = f'transform="translate({_num(x)} {_num(y)}) scale({_num(k)})"'
    d = {
        "download": f'<path d="M8 2.5V11M4.5 7.5L8 11l3.5-3.5M3 13.5h10" {st}/>',
        "check": f'<path d="M3 8.5l3.2 3.2L13 4.8" {st}/>',
        "bluetooth": f'<path d="M4.5 5l7 6-3.5 3V2l3.5 3-7 6" {st}/>',
        "camera": f'<path d="M2.5 5.5h2.6l1.2-1.8h3.4l1.2 1.8h2.6v7h-11z" {st}/><circle cx="8" cy="8.9" r="2.2" {st}/>',
        "speaker": f'<path d="M2.5 6h2.5l3.5-3v10L5 10H2.5z" {st}/><path d="M11 5.5a3.5 3.5 0 0 1 0 5M12.8 3.6a6 6 0 0 1 0 8.8" {st}/>',
        "battery": f'<rect x="1.5" y="4.5" width="11.5" height="7" rx="1.6" {st}/><path d="M14.6 7v2" {st}/>'
                   f'<rect x="3.2" y="6.2" width="3" height="3.6" rx="0.6" fill="{color}"/>',
        "focus": f'<circle cx="8" cy="8.5" r="5.5" {st}/><path d="M8 5.5v3l2 1.5M6.5 1.8h3" {st}/>',
        "prev": f'<path d="M4.5 3.5v9M12 3.8L6.8 8l5.2 4.2z" {st}/>',
        "next": f'<path d="M11.5 3.5v9M4 3.8L9.2 8 4 12.2z" {st}/>',
        "pause": f'<path d="M5.8 3.8v8.4M10.2 3.8v8.4" {st}/>',
    }[name]
    return f'<g {T}>{d}</g>'

def cursor(clip, keys, start):
    """A pointer moving between (x, y) keys; keys are [(t, (x, y))]."""
    pts = [(t, f"{_num(x)} {_num(y)}") for t, (x, y) in keys]
    a = clip.anim("transform", pts, f"{_num(start[0])} {_num(start[1])}")
    a = a.replace("<animate ", '<animateTransform type="translate" ', 1)
    return (f'<g transform="translate({_num(start[0])} {_num(start[1])})">{a}'
            '<path d="M0 0v15.5l3.7-3.4 2.4 5.6 2.4-1-2.4-5.5h5.1z" fill="#fff" stroke="#14161B" stroke-width="1.1" '
            'stroke-linejoin="round"/></g>')

def scene(w, h, body, defs="", title=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w * 3 // 2}" height="{h * 3 // 2}" role="img">'
            f'<title>{title}</title><defs>'
            '<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#7FA9D6"/>'
            '<stop offset="0.6" stop-color="#A9C4E4"/><stop offset="1" stop-color="#C8C3E6"/></linearGradient>'
            '<radialGradient id="glow" cx="0.5" cy="0" r="0.75"><stop offset="0" stop-color="#fff" stop-opacity="0.35"/>'
            '<stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>'
            f'<filter id="shadow" filterUnits="userSpaceOnUse" x="0" y="0" width="{w}" height="{h}">'
            '<feDropShadow dx="0" dy="8" stdDeviation="10" flood-color="#0B1220" flood-opacity="0.32"/></filter>'
            f'{ART_DEF}{defs}</defs>'
            f'<rect width="{w}" height="{h}" rx="16" fill="url(#bg)"/><rect width="{w}" height="{h}" rx="16" fill="url(#glow)"/>'
            f'{body}</svg>')

def pill(clip, cx, events):
    """The island's glass shape, centered on cx, its top TOP below the screen edge, sprung between sizes."""
    (w0, h0, r0), ks = morph(events)
    xk = [(t, cx - v / 2) for t, v in ks["w"]]
    return (f'<rect x="{_num(cx - w0 / 2)}" y="{TOP}" width="{_num(w0)}" height="{h0}" rx="{r0}" fill="{GLASS}" fill-opacity="0.94" '
            f'stroke="#fff" stroke-opacity="0.14" filter="url(#shadow)">'
            + clip.anim("x", xk, cx - w0 / 2) + clip.anim("width", ks["w"], w0)
            + clip.anim("height", ks["h"], h0) + clip.anim("rx", ks["r"], r0) + clip.anim("ry", ks["r"], r0) + "</rect>")

def group(clip, shows, inner, start_visible):
    keys = fades(shows)
    return (f'<g opacity="{1 if start_visible else 0}">' + clip.anim("opacity", keys, 1 if start_visible else 0) + inner + "</g>")

def collapsed_w(content, pad=12):
    return min(260, max(124, math.ceil((content + 2 * pad) / 4) * 4))

# ---------------------------------------------------------------- rest line pieces
CLOCK = "Thu Nov 13  12:59 PM"
def rest_row(x, cy, clip=None, cpu_keys=None, glyph=None, glyph_inner=""):
    """[status slot][clock][bars] from x; returns (svg, width)."""
    s, cx = [], x
    if glyph is not None:
        s.append(glyph_inner); cx += 16 + 8
    cw = text_w(CLOCK, 12)
    s.append(text(CLOCK, cx, cy, 12, 400, INK)); cx += cw + 8
    s.append(usage_bars(cx, cy, 0.42, 0.64, clip, cpu_keys)); cx += 26
    return "".join(s), cx - x

def rest_width(glyph=False):
    return (24 if glyph else 0) + text_w(CLOCK, 12) + 8 + 26

# ---------------------------------------------------------------- hero: rest -> song peek -> tuck -> hover card -> rest
def hero():
    W, H, D = 440, 212, 15.0
    c = Clip(D); cx = W / 2; cy = TOP + COLLAPSED_H / 2
    title, artist = "Harbor Lights", "Mara Vale"
    rest_w = collapsed_w(rest_width())
    peek_c = 22 + 10 + text_w(title, 12, 700) + 6 + text_w("· " + artist, 12) + 10 + EQ_W
    peek_w = collapsed_w(peek_c, 7)
    tuck_c = 22 + 10 + rest_width() + 10 + EQ_W
    tuck_w = collapsed_w(tuck_c, 7)
    t_peek, t_tuck, t_open, t_close, t_end = 1.6, 5.4, 7.6, 12.2, 13.9
    events = [(0, rest_w, COLLAPSED_H, COLLAPSED_R), (t_peek, peek_w, COLLAPSED_H, COLLAPSED_R),
              (t_tuck, tuck_w, COLLAPSED_H, COLLAPSED_R), (t_open, EXPANDED_W, EXPANDED_H, EXPANDED_R),
              (t_close, tuck_w, COLLAPSED_H, COLLAPSED_R), (t_end, rest_w, COLLAPSED_H, COLLAPSED_R)]
    body = [pill(c, cx, events)]
    cpu = [(2.0, 0.55), (4.0, 0.38), (6.0, 0.47), (9.0, 0.6), (11.0, 0.41), (13.5, 0.42)]

    # rest line
    x0 = cx - rest_width() / 2
    row, _ = rest_row(x0, cy, c, cpu)
    body.append(group(c, [(0, t_peek), (t_end, None)], row, True))
    # song peek
    x = cx - peek_c / 2
    p = [art(x, cy - 11, 22, 6, "art")]
    x += 32; p.append(text(title, x, cy, 12, 700)); x += text_w(title, 12, 700) + 6
    p.append(text("· " + artist, x, cy, 12, 400, INK, INK2_A)); x += text_w("· " + artist, 12) + 10
    p.append(eq(x, cy))
    body.append(group(c, [(t_peek, t_tuck)], "".join(p), False))
    # tucked: art, clock, bars, equalizer
    x = cx - tuck_c / 2
    tk = [art(x, cy - 11, 22, 6, "art")]
    r, rw = rest_row(x + 32, cy, c, cpu); tk.append(r)
    tk.append(eq(x + 32 + rw + 10, cy))
    body.append(group(c, [(t_tuck, t_open), (t_close, t_end)], "".join(tk), False))
    # media card
    ox, oy = cx - EXPANDED_W / 2 + PAD_X, TOP + PAD_Y
    iw = EXPANDED_W - 2 * PAD_X
    m = [art(ox, oy, 62, 14, "art")]
    m.append(text(title, ox + 76, oy + 21, 16, 700)); m.append(text(artist, ox + 76, oy + 42, 13, 400, INK, INK2_A))
    by = oy + 80
    m.append(text("1:12", ox, by, 11, 500, INK, INK2_A)); m.append(text("-2:03", ox + iw, by, 11, 500, INK, INK2_A, "end"))
    bx, bw = ox + 44, iw - 88
    played = bw * 0.36
    # wavy played part: a long wave drifting left inside a clip, then a gap and the flat track
    wl, amp = 32, 3
    pts = [f"M{_num(bx - wl * 2)} {_num(by)}"]
    xx = bx - wl * 2
    while xx < bx + played + wl:
        pts.append(f"q{wl / 4} {-amp * 1.33:.2f} {wl / 2} 0 t{wl / 2} 0"); xx += wl
    wave = (f'<clipPath id="played"><rect x="{_num(bx - 2)}" y="{_num(by - 8)}" width="{_num(played + 2)}" height="16"/></clipPath>'
            f'<g clip-path="url(#played)"><path d="{" ".join(pts)}" fill="none" stroke="{ACCENT}" stroke-width="4" stroke-linecap="round">'
            f'<animateTransform attributeName="transform" type="translate" from="0 0" to="{wl} 0" dur="1.6s" repeatCount="indefinite"/></path></g>')
    m.append(wave)
    m.append(f'<path d="M{_num(bx + played + 6)} {_num(by)}H{_num(bx + bw)}" stroke="{INK}" stroke-opacity="{TRACK_A}" stroke-width="4" stroke-linecap="round"/>')
    tcy = oy + EXPANDED_H - 2 * PAD_Y - 17
    for i, name in enumerate(("prev", "pause", "next")):
        bcx = ox + iw / 2 + (i - 1) * 48
        if name == "pause":
            m.append(f'<circle cx="{_num(bcx)}" cy="{_num(tcy)}" r="17" fill="{ACCENT}"/>' + icon(name, bcx - 8, tcy - 8, ON_ACCENT, 2))
        else:
            m.append(f'<circle cx="{_num(bcx)}" cy="{_num(tcy)}" r="17" fill="{INK}" fill-opacity="{BTN_A}"/>' + icon(name, bcx - 8, tcy - 8, INK, 1.5))
    for label, val, lx in (("CPU", "41%", ox), ("RAM", "64%", ox + iw - 90)):
        m.append(text(label, lx, tcy, 12, 400, INK, INK2_A))
        bx2 = lx + text_w("RAM", 12) + 6; bw2 = 90 - (bx2 - lx) - 6 - text_w("100%", 12, 600)
        m.append(rrect(bx2, tcy - 2, bw2, 4, 2, INK, TRACK_A)); m.append(rrect(bx2, tcy - 2, bw2 * int(val[:-1]) / 100, 4, 2, ACCENT))
        m.append(text(val, lx + 90, tcy, 12, 600, INK, None, "end"))
    body.append(group(c, [(t_open, t_close)], "".join(m), False))
    # pointer: comes in, rests on the island while it is open, leaves
    off, on = (cx + 120, 170), (cx + 46, 36)
    body.append(cursor(c, [(t_open - 0.7, off), (t_open - 0.05, on), (t_close - 0.6, on), (t_close + 0.25, (cx + 110, 200)),
                           (t_close + 0.26, (cx + 400, 300)), (D - 0.01, (cx + 400, 300))], (cx + 400, 300)))
    return scene(W, H, "".join(body), title="Statlet: a song peeks in, tucks beside the clock, and opens into the media card on hover")

# ---------------------------------------------------------------- activities: the rest line's status slot
def activities():
    W, H, D = 440, 80, 11.0
    c = Clip(D); cx = W / 2; cy = TOP + COLLAPSED_H / 2
    w0, w1 = collapsed_w(rest_width()), collapsed_w(rest_width(True))
    steps = [("download", 0.8, 3.4), ("check", 3.4, 4.8), ("bluetooth", 5.6, 7.6), ("camera", 8.4, 9.9)]
    events = [(0, w0, COLLAPSED_H, COLLAPSED_R), (0.8, w1, 34, 11), (4.9, w0, 34, 11), (5.6, w1, 34, 11), (7.7, w0, 34, 11),
              (8.4, w1, 34, 11), (10.0, w0, 34, 11)]
    body = [pill(c, cx, events)]
    x_plain, x_slot = cx - rest_width() / 2, cx - rest_width(True) / 2
    r, _ = rest_row(x_plain, cy)
    body.append(group(c, [(0, 0.8), (4.9, 5.6), (7.7, 8.4), (10.0, None)], r, True))
    r, _ = rest_row(x_slot, cy, glyph=True)  # clock and bars, moved over to make room for the slot
    body.append(group(c, [(0.8, 4.9), (5.6, 7.7), (8.4, 10.0)], r, False))
    for name, a, b in steps:
        g = icon(name, x_slot + 1, cy - 8)
        if name == "download":  # breathes while the file downloads (breatheMs 1600)
            g = f'<g><animate attributeName="opacity" values="1;0.4;1" dur="1.6s" repeatCount="indefinite"/>{g}</g>'
        body.append(group(c, [(a, b)], g, False))
    return scene(W, H, "".join(body), title="Statlet's rest line showing a download, a finished download, a Bluetooth connection and a screenshot")

# ---------------------------------------------------------------- takeover cards
def cards():
    W, H, D = 440, 140, 12.0
    c = Clip(D); cx = W / 2
    w0 = collapsed_w(rest_width()); cy = TOP + COLLAPSED_H / 2
    events = [(0, w0, 34, 11), (0.8, EXPANDED_W, CARD_H, EXPANDED_R), (3.6, w0, 34, 11), (4.6, EXPANDED_W, CARD_H, EXPANDED_R),
              (8.1, w0, 34, 11), (8.9, EXPANDED_W, CARD_H, EXPANDED_R), (11.0, w0, 34, 11)]
    body = [pill(c, cx, events)]
    r, _ = rest_row(cx - rest_width() / 2, cy)
    body.append(group(c, [(0, 0.8), (3.6, 4.6), (8.1, 8.9), (11.0, None)], r, True))
    ox, oy, iw, ih = cx - EXPANDED_W / 2 + PAD_X, TOP + PAD_Y, EXPANDED_W - 2 * PAD_X, CARD_H - 2 * PAD_Y

    def card(ic, title, detail, value, frac, a, b, keys=None, bar_color=ACCENT):
        s = [icon(ic, ox, oy + 3, bar_color), text(title, ox + 26, oy + 11, 16, 700)]
        if frac is None:  # no bar: the detail sits under the title
            s.append(text(detail, ox, oy + 38, 13, 400, INK, INK2_A))
            return group(c, [(a, b)], "".join(s), False)
        s.append(text(detail, ox + iw, oy + 11, 13, 400, INK, INK2_A, "end"))
        rowy = oy + ih - 8
        bw = iw - 52
        s.append(rrect(ox, rowy - 3, bw, 6, 3, INK, TRACK_A))
        fill = rrect(ox, rowy - 3, bw * frac, 6, 3, bar_color)
        if keys:
            fill = fill[:-2] + ">" + c.anim("width", [(t, bw * v) for t, v in keys], bw * frac) + "</rect>"
            # the value counts along with the bar: one label per step
            for i, (t, v) in enumerate([(a, frac)] + keys):
                t_out = keys[i][0] if i < len(keys) else b
                lab = text(f"{round(v * 100)}%", ox + iw, rowy, 13, 600, INK, None, "end")
                s.append(f'<g opacity="0">' + c.anim("opacity", [(t, 1), (t_out - 0.001, 1), (t_out, 0)], 0) + lab + "</g>")
        else:
            s.append(text(value, ox + iw, rowy, 13, 600, INK, None, "end"))
        s.append(fill)
        return group(c, [(a, b)], "".join(s), False)

    vol = [(1.5, 0.46), (1.8, 0.50), (2.1, 0.54), (2.4, 0.58)]
    body.append(card("speaker", "Volume", "Speakers", None, 0.42, 0.8, 3.6, vol))
    body.append(card("battery", "Battery low", "About 40 min left", "18%", 0.18, 4.6, 8.1, bar_color="#FFB020"))
    body.append(card("focus", "Focus started", "25 min, one thing at a time", None, None, 8.9, 11.0))
    return scene(W, H, "".join(body), title="Statlet takeover cards for volume, low battery and a Focus timer")

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, fn in (("hero.svg", hero), ("activities.svg", activities), ("cards.svg", cards)):
        svg = fn()
        with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(svg)
        print(name, len(svg) // 1024, "KB")
