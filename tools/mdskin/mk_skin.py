#!/usr/bin/env python3
"""Builds the MPC skin for Machinedrum One: mpc-vst-monomodule's own vst/skin/mk_skin.py (at 56cb8e0, its "2x2"
layout), adapted to the Machinedrum - the same LCD look, drawing code, knob cells, machine bar, picker and LEV
column, not a new design. Upstream-of-this differences are only what the Machinedrum needs:

  * 16 tabs, one per track (the Monomodule has one voice). Each tab is Monomodule's 2x2: SYN | AMP/EFX over
    ROUTE | the track's LEV column (Monomodule's GLOBAL quadrant with LEV, minus its extra globals).
  * Machines, labels: the MD OS's own machine table (tools/mdmachine's listing), 131 machines in 9 groups; the
    picker's columns are the groups (ROM split into columns of 16 so no column scrolls).
  * The machine param is a raw OS machine id (0-191), so IndexedEnabling / button ids use the id, n = 192.
  * SYN cells: one set of dials per track (not per machine - 131 x 16 would be ~33k components); the per-machine
    label grid is an overlay drawn over them, opaque except over its own live cells, so an unused cell is blank.
  * No preset/bank strip (the MD port has no presets) and no Shnolk logo (the vendor's own; "MD" in its place).

The LCD artwork (fonts, dial ring and dot) comes from mpc-vst-monomodule's art.json (its mnm-artdump of the
user's own Monomachine OS: same Elektron LCD family). Per-user build output, never committed or distributed.

    mk_skin.py <art.json> <mdmachine listing> <params.json> <out-dir> [skin=... ink=RRGGBB paper=RRGGBB]
"""
import json
import os
import re
import sys

from PIL import Image, ImageDraw, ImageOps

TOOLS = os.environ.get("MPC_VST_TOOLS") or os.path.join(os.path.expanduser("~"), "mpc-vst", "tools")
sys.path.insert(0, TOOLS)
import shadow_skin as ss  # noqa: E402  (TUI.json helpers shared with the other ports)

args = dict(a.split("=", 1) for a in sys.argv[5:])
PRESETS = {"default": ("000000", "ffffff"), "inverted": ("ffffff", "000000"), "lowcontrast": ("5c5c5c", "c4c4c4"),
           # the Machinedrum's own LCD: near-black ink on its orange-red backlight (this port's default)
           "md": ("1a0800", "ff5a1f"),
           "red": ("ff3b2e", "1c0403"), "blue": ("5ab0ff", "04112b"), "green": ("52ff70", "031608"), "orange": ("ffa11f", "1e1000")}
_skin = args.get("skin", "md")
_swap = _skin.endswith("-inverted") and _skin != "-inverted"
_ink, _paper = PRESETS.get(_skin[:-9] if _swap else _skin, PRESETS["default"])
if _swap:
    _ink, _paper = _paper, _ink
_ink, _paper = args.get("ink", _ink), args.get("paper", _paper)

# Monomodule's "2x2" geometry, unchanged
S = int(args.get("scale", "3"))
SKIN_W, SKIN_H = 1280, 628
CELL, LABEL_Y, CONTENT_Y, CONTENT_H, VALUE_Y, VALUE_H = 32, 3, 9, 14, 23, 9
TITLE_H, GRID_Y = 10, 11
MARG, GAPX, PAGE_GAP = 10, 8, 12
BAR_ROWS = 26
ROW_GAP = PAGE_GAP
CW = int(args.get("cellw", "40"))
LCD_W = 4 * CW + 1
PAGE_GAP = MARG = int(args.get("gap", (SKIN_W - 2 * LCD_W * S) // 3))
PAGE_LCD_H = GRID_Y + 2 * CELL + 1
PAGE_ROWS = 2
LEV_W = 19 * S
PAGES_W = 2 * LCD_W * S + PAGE_GAP
BAR_X_W = MARG + LEV_W + GAPX
TOP = 8 + BAR_ROWS * S + 8
PAGES_X0 = MARG
WIN_W = PAGES_X0 + PAGES_W + MARG
WIN_H = TOP + PAGE_ROWS * PAGE_LCD_H * S + (PAGE_ROWS - 1) * ROW_GAP + 8
OX, OY = (SKIN_W - WIN_W) // 2, (SKIN_H - WIN_H) // 2
FRAMES = 128
TRACKS = 16
NMACH = 192   # the machine param's range (raw OS ids 0-191)

INK = tuple(int(_ink[i:i + 2], 16) for i in (0, 2, 4))
PAPER = tuple(int(_paper[i:i + 2], 16) for i in (0, 2, 4))


# ---------------------------------------------------------------- art (Monomodule, verbatim) -----------------------
class Bmp:
    def __init__(self, d):
        self.w, self.h = d["w"], d["h"]
        self.rows = [int(r, 16) for r in d["rows"]]

    def lit(self, x, y):
        return (self.rows[y] >> (63 - x)) & 1


class Font:
    def __init__(self, d):
        self.h, self.adv = d["h"], d["adv"]
        self.g = {int(k): Bmp(v) for k, v in d["glyphs"].items()}


art = json.load(open(sys.argv[1]))
F = {k: Font(v) for k, v in art["fonts"].items()}
B = art["bitmaps"]
DIAL_RING, GROUP_TIE, RING_PLAIN = Bmp(B["dialRing"]), Bmp(B["groupTie"]), Bmp(B["ringPlain"])
DIAL_DOT = [Bmp(b) for b in B["dialDot"]]


class Canvas:
    """1-bit LCD canvas: on = ink."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h)

    def set(self, x, y, on=True):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = 1 if on else 0

    def blit(self, b, x, y, on=True):
        for r in range(b.h):
            row = b.rows[r]
            for c in range(b.w):
                if (row >> (63 - c)) & 1:
                    self.set(x + c, y + r, on)

    def fill(self, x, y, w, h, on=True):
        for r in range(h):
            for c in range(w):
                self.set(x + c, y + r, on)

    def dots_h(self, x0, x1, y):
        for x in range(x0, x1 + 1, 2):
            self.set(x, y)

    def dots_v(self, x, y0, y1):
        for y in range(y0, y1 + 1, 2):
            self.set(x, y)

    def text(self, font, s, x, y, on=True):
        for ch in s:
            g = font.g.get(ord(ch))
            if g:
                self.blit(g, x, y, on)
                x += g.w + 1
            else:
                x += font.adv + 1

    def text_centred(self, font, s, x0, w, y, on=True):
        self.text(font, s, x0 + (w - text_width(font, s)) // 2, y, on)

    def image(self, scale=S):
        im = Image.frombytes("L", (self.w, self.h), bytes(0 if p else 255 for p in self.px))
        im = im.resize((self.w * scale, self.h * scale), Image.NEAREST)
        return ImageOps.colorize(im, black=INK, white=PAPER)


def text_width(font, s):
    w = 0
    for ch in s:
        g = font.g.get(ord(ch))
        w += (g.w if g else font.adv) + 1
    return max(0, w - 1)


class P:
    """a knob cell (Monomodule's spec::Param, reduced to what the MD's cells use: dials, no list icons)"""

    def __init__(self, label, display="numeric", default=0):
        self.label, self.display, self.default = label, display if label else "blank", default
        self.tie, self.max = 0, 127


def value_text(p, raw):
    raw = max(0, min(p.max, raw))
    if p.display == "bipolar":
        b = raw - 64
        return "+%d" % b if b > 0 else str(b)
    return str(raw)


def cell_static(cv, x0, y0, p):
    """dotted top/left border and label: the parts of a knob cell that never change."""
    cv.dots_h(x0, x0 + CW, y0)
    cv.dots_v(x0, y0, y0 + CELL - 1)
    if p.display == "blank":
        return
    cv.text_centred(F["tiny3x5"], p.label, x0 + 1, CW - 1, y0 + LABEL_Y)


def cell_dynamic(cv, x0, y0, p, raw):
    """dial plus the value row (upstream drawKnobCell minus border/label); (x0, y0) = the cell origin."""
    inner_x, inner_w = x0 + 1, CW - 1
    rx, ry = inner_x + (inner_w - DIAL_RING.w) // 2, y0 + CONTENT_Y + (CONTENT_H - DIAL_RING.h) // 2
    cv.blit(DIAL_RING, rx, ry)
    cv.blit(DIAL_DOT[raw], rx + 2, ry + 4)
    font = F["tiny3x5"]
    bx, by, bw, bh = inner_x, y0 + VALUE_Y, CW - 1, VALUE_H
    cv.text_centred(font, value_text(p, raw), bx, bw, by + (bh - font.h) // 2)


FR_X, FR_Y, FR_W, FR_H = 1, CONTENT_Y, CW - 1, CELL - CONTENT_Y


# ------------------------------------------------------------------ output ----------------------------------------
NAME, VENDOR = "Machinedrum One", "sd88me"
OUT = os.path.join(sys.argv[4], "%s - VST - %s" % (VENDOR, NAME))
SKIN = os.path.join(OUT, "Plugin Skins")
os.makedirs(SKIN, exist_ok=True)
for f in os.listdir(SKIN):
    os.remove(os.path.join(SKIN, f))
params = json.load(open(sys.argv[3]))["params"]
PIDX = {p["key"]: i for i, p in enumerate(params)}


def save_png(name, im):
    fn = name + ".png"
    im.save(os.path.join(SKIN, fn), optimize=True)
    return fn


def strip_image(p, cache={}):
    """128 frames of the dynamic part of a cell of this kind, stacked down."""
    k = p.display
    if k in cache:
        return cache[k]
    frames = []
    for raw in range(FRAMES):
        cv = Canvas(FR_W, FR_H)
        cell_dynamic(cv, -FR_X, -FR_Y, p, raw)
        frames.append(cv.image())
    fw, fh = frames[0].size
    st = Image.new("RGB", (fw, fh * FRAMES))
    for i, f in enumerate(frames):
        st.paste(f, (0, i * fh))
    cache[k] = (save_png("st_%s" % k, st), fw, fh)
    return cache[k]


defs = {}
TABK = [[] for _ in range(TRACKS)]   # components per tab (= per track)
PREVIEW = []   # (image file, x, y, w, h, condition, frame index or None, tab), in draw order, for the offline composite


def knob_def(fn, w, h, orient="Vertical", interactive=True):
    """interactive=False: draws the filmstrip only; a separate touch overlay (touch_knob) owns the Q-Link binding
    (two Knob components on one Parameter confuse MPC's Q-Link handling - see Monomodule's HANDOFF/NOTES)."""
    key = "mdKnob_%s%s" % (fn[:-4], "" if interactive else "_d")
    if key not in defs:
        actions = [ss._action("Mouse Down", "Q-Link"), ss._action("Double Click", "Show Overlay", "knob overlay"),
                   ss._action("Enter Pressed", "Show Overlay", "knob overlay")] if interactive else []
        children = ([ss._focus(w, h)] if interactive else []) + [
            ss._sub("Knob", {"version": 5, "knobType": "FilmStrip", "filmStrip": fn, "numFrames": FRAMES - 1,
                             "invert": False, "dragOrientation": orient, "handleName": "Data"},
                    ss._bounds(0, 0, w, h), "Knob")]
        defs[key] = ss._local(key, actions, children)
    return key


def place(ctype, name, index, x, y, w, h, tab, focus="No", cond=None, extra=None, img=None, raw=None):
    if img:
        PREVIEW.append((img, x, y, w, h, cond, raw, tab))
    m = [{"key": "Data", "value": "Parameter %d" % index}]
    for hn, hi in (extra or {}).items():
        m.append({"key": hn, "value": "Parameter %d" % hi})
    b = ss._bounds(x, y, w, h, focus=focus, show="Show" if cond else "Hide")
    if cond:
        b["additionalInvalidatingHandles"] = [cond]
    TABK[tab].append({"version": 2, "componentData": {"version": 1, "name": name, "type": ctype, "data": {"version": 1, "handleName": "Data"}},
                      "handle remapping": {"version": 1, "map": m}, "bounds": b})


def image_comp(name, fn, x, y, w, h, tab, cond=None):
    PREVIEW.append((fn, x, y, w, h, cond, None, tab))
    b = ss._bounds(x, y, w, h, show="Show")
    if cond:
        b["additionalInvalidatingHandles"] = [cond]
    TABK[tab].append(ss._sub("Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": fn}, b, name))


def enabling(key, i, n):
    return "IndexedEnabling/%d/%d/Parameter %d" % (i, n, PIDX[key])


# ------------------------------------------------------------ page geometry (window coords -> skin px) --------------
PAGE_POS = {"SYN": (0, 0), "AMP": (1, 0), "ROUTE": (0, 1), "TRACK": (1, 1)}
PAGE_TITLE = {"SYN": "SYN", "AMP": "AMP/EFX", "ROUTE": "ROUTE", "TRACK": None}   # TRACK: "TRACK <n>", per tab


def page_origin(name):
    col, row = PAGE_POS[name]
    return OX + PAGES_X0 + col * (LCD_W * S + PAGE_GAP), OY + TOP + row * (PAGE_LCD_H * S + ROW_GAP)


def page_canvas(name, cells, title=None):
    """A page's static drawing: title bar, dotted cell borders + labels, grid right/bottom edge. cells: 8 P."""
    cv = Canvas(LCD_W, PAGE_LCD_H)
    cv.fill(0, 0, LCD_W, TITLE_H, True)
    cv.text(F["bold8"], title or PAGE_TITLE[name], 2, 1, False)
    for k, p in enumerate(cells):
        cell_static(cv, (k % 4) * CW, GRID_Y + (k // 4) * CELL, p)
    cv.dots_v(LCD_W - 1, GRID_Y, PAGE_LCD_H - 1)
    cv.dots_h(0, LCD_W - 1, PAGE_LCD_H - 1)
    return cv


def px_text(im, font, s, x, y, scale, colour):
    """LCD-font text at screen resolution (any integer scale) onto an RGB image."""
    dr = ImageDraw.Draw(im)
    for ch in s:
        g = font.g.get(ord(ch))
        if not g:
            x += (font.adv + 1) * scale
            continue
        for r in range(g.h):
            for c in range(g.w):
                if g.lit(c, r):
                    dr.rectangle([x + c * scale, y + r * scale, x + (c + 1) * scale - 1, y + (r + 1) * scale - 1], fill=colour)
        x += (g.w + 1) * scale


# ------------------------------------------------------------------ machines -----------------------------------------
def parse_machines(path):
    """tools/mdmachine's listing: '%3d %-6s fn=$%06x' then the 8 SYN labels joined by single spaces (blank = empty)."""
    out = []
    for line in open(path):
        m = re.match(r"\s*(\d+) (\S+)\s+fn=\$[0-9a-f]+ (.*)$", line.rstrip("\n"))
        if not m:
            continue
        labels = (m.group(3).split(" ") + [""] * 8)[:8] if m.group(3) else [""] * 8
        name = m.group(2)
        out.append({"id": int(m.group(1)), "name": name, "group": name[:3], "short": name[3:], "labels": labels})
    return out


MACHINES = parse_machines(sys.argv[2])
GROUP_TITLE = {"GND": "GND", "P-I": "P-I", "TRX": "TRX", "EFM": "EFM", "E12": "E12", "INP": "INP", "MID": "MID", "CTR": "CTR", "ROM": "ROM", "RAM": "RAM"}
BAR_H, LOGO_X, LOGO_GAP, ARROW_GAP, ARROW_W, PAD_R = 26, 4, 6, 4, 5, 4


def machine_bar(m):
    """Monomodule's machine_bar (upstream MachineBar::paint) with the group name in place of a group logo."""
    bold = F["bold8"]
    grp = GROUP_TITLE.get(m["group"], m["group"])
    logo_w = text_width(bold, grp)
    name_x = LOGO_X + logo_w + LOGO_GAP
    w = name_x + text_width(bold, m["short"]) + ARROW_GAP + ARROW_W + PAD_R
    cv = Canvas(w, BAR_H)
    cv.fill(0, 0, w, BAR_H, True)
    cv.text(bold, grp, LOGO_X, (BAR_H - bold.h) // 2, False)
    cv.text(bold, m["short"], name_x, (BAR_H - bold.h) // 2, False)
    ax, ay = name_x + text_width(bold, m["short"]) + ARROW_GAP, BAR_H // 2 - 1
    for r in range(3):
        half = 2 - r
        for c in range(2 - half, 2 + half + 1):
            cv.set(ax + c, ay + r, False)
    return cv.image()


PREVIEW_MACHINE = int(args.get("machine", "16"))   # TRXBD
# ------------------------------------------------------------------ cells ---------------------------------------------
AMP_CELLS = [P("AMD"), P("AMF"), P("EQF", default=64), P("EQG", "bipolar", 64), P("FLTF"), P("FLTW", default=127), P("FLTQ"), P("SRR")]
AMP_KEYS = ["amd", "amf", "eqf", "eqg", "fltf", "fltw", "fltq", "srr"]
ROUTE_CELLS = [P("DIST"), P("VOL", default=100), P("PAN", "bipolar", 64), P("DEL"), P("REV"), P("LFOS"), P("LFOD"), P("LFOM")]
ROUTE_KEYS = ["dist", "vol", "pan", "del", "rev", "lfos", "lfod", "lfom"]
BLANK8 = [P("") for _ in range(8)]


def syn_cells(m):
    return [P(l) for l in m["labels"]]


default_machine = next(m for m in MACHINES if m["id"] == PREVIEW_MACHINE)

# backgrounds: one per track (the TRACK quadrant's title names it); SYN uses the default machine's labels here, the
# per-machine overlay repaints its grid
EMPTY_MACHINE = next(m for m in MACHINES if m["id"] == 0)   # GND--: a new track's machine
bgs = []
for t in range(TRACKS):
    b_ = Image.new("RGB", (SKIN_W, SKIN_H), PAPER)
    px_text(b_, F["bold8"], "MD", OX + MARG + (LEV_W - text_width(F["bold8"], "MD") * 4) // 2, OY + 8 + (BAR_ROWS * S - F["bold8"].h * 4) // 2, 4, INK)
    for name, cells in (("SYN", syn_cells(EMPTY_MACHINE)), ("AMP", AMP_CELLS), ("ROUTE", ROUTE_CELLS), ("TRACK", BLANK8)):
        cv = page_canvas(name, cells, "TRACK %d" % (t + 1) if name == "TRACK" else None)
        b_.paste(cv.image(), page_origin(name))
    bgs.append(b_)

# LEV: Monomodule's LEVQ column (label, dotted frame, solid bar) in the first cell column of the TRACK quadrant
qx, qy = page_origin("TRACK")
col_rows = 2 * CELL
lv = Canvas(CW, col_rows)
lv.dots_h(0, CW - 1, 0); lv.dots_v(0, 0, col_rows - 1)
lv.text_centred(F["bold8"], "LEV", 0, CW, 1)
fx_, fy_, fw_l, fh_l = (CW - 19) // 2, 10, 19, col_rows - 11
lv.dots_h(fx_, fx_ + fw_l - 1, fy_); lv.dots_h(fx_, fx_ + fw_l - 1, fy_ + fh_l - 1)
lv.dots_v(fx_, fy_, fy_ + fh_l - 1); lv.dots_v(fx_ + fw_l - 1, fy_, fy_ + fh_l - 1)
for b_ in bgs:
    b_.paste(lv.image(), (qx, qy + GRID_Y * S))
for t, b_ in enumerate(bgs):
    image_comp("Background", save_png("bg_%02d" % t, b_), 0, 0, SKIN_W, SKIN_H, tab=t)

BAR_X, BAR_Y = OX + BAR_X_W, OY + 8

# knob cells
TOUCH_INSET = 2
TOUCH_W, TOUCH_H = (CW - 2 * TOUCH_INSET) * S, (CELL - 2 * TOUCH_INSET) * S
_touch_png = []


def touch_file():
    if not _touch_png:
        _touch_png.append(save_png("touch", Image.new("RGBA", (8, 8 * FRAMES), (0, 0, 0, 0))))
    return _touch_png[0]


def touch_knob(name, index, x0, y0, tab):
    kd = knob_def(touch_file(), TOUCH_W, TOUCH_H)
    place(kd, "%s touch" % name, index, x0 + TOUCH_INSET * S, y0 + TOUCH_INSET * S, TOUCH_W, TOUCH_H, tab)


def cell_dials(page, cells, keys, t):
    """the dial strips of a page's live cells (display only)"""
    x0, y0 = page_origin(page)
    for k, p in enumerate(cells):
        if p.display == "blank":
            continue
        fn, fw, fh = strip_image(p)
        kx = x0 + ((k % 4) * CW + FR_X) * S
        ky = y0 + (GRID_Y + (k // 4) * CELL + FR_Y) * S
        place(knob_def(fn, fw, fh, interactive=False), "%s %s" % (page, p.label or k), PIDX[keys[k]], kx, ky, fw, fh, t, img=fn, raw=p.default)


def cell_touch(page, cells, keys, t):
    x0, y0 = page_origin(page)
    for k, p in enumerate(cells):
        if p.display != "blank":
            touch_knob("%s %d" % (page, k + 1), PIDX[keys[k]], x0 + (k % 4) * CW * S, y0 + (GRID_Y + (k // 4) * CELL) * S, t)


# SYN: every dial numeric; the per-machine overlay (below) blanks the cells a machine doesn't use
SYN_ANY = [P("SYN%d" % (k + 1)) for k in range(8)]


def syn_overlay(m, cache={}):
    """The SYN grid for a machine (labels, borders), transparent over its live cells' dial areas so the dials under it
    show; opaque over unused cells, which hides their dials. Shared by every machine with the same label set."""
    key = tuple(m["labels"])
    if key in cache:
        return cache[key]
    cells = syn_cells(m)
    cv = page_canvas("SYN", cells)
    im = cv.image().crop((0, GRID_Y * S, LCD_W * S, PAGE_LCD_H * S)).convert("RGBA")
    dr = ImageDraw.Draw(im)
    for k, p in enumerate(cells):
        if p.display == "blank":
            continue
        x = ((k % 4) * CW + FR_X) * S
        y = ((k // 4) * CELL + FR_Y) * S
        dr.rectangle([x, y, x + FR_W * S - 1, y + FR_H * S - 1], fill=(0, 0, 0, 0))
    cache[key] = save_png("syn_%02d" % len(cache), im)
    return cache[key]


for t in range(TRACKS):
    tk = lambda k: "track%d_%s" % (t, k)
    syn_keys = [tk("syn%d" % (k + 1)) for k in range(8)]
    cell_dials("SYN", SYN_ANY, syn_keys, t)
    sx, sy = page_origin("SYN")
    for m in MACHINES:
        image_comp("SYN grid %s" % m["name"], syn_overlay(m), sx, sy + GRID_Y * S, LCD_W * S, (PAGE_LCD_H - GRID_Y) * S, t,
                   cond=enabling(tk("machine"), m["id"], NMACH))
    cell_touch("SYN", SYN_ANY, syn_keys, t)
    cell_dials("AMP", AMP_CELLS, [tk(k) for k in AMP_KEYS], t)
    cell_touch("AMP", AMP_CELLS, [tk(k) for k in AMP_KEYS], t)
    cell_dials("ROUTE", ROUTE_CELLS, [tk(k) for k in ROUTE_KEYS], t)
    cell_touch("ROUTE", ROUTE_CELLS, [tk(k) for k in ROUTE_KEYS], t)

    # machine bar, one image per machine, shown while it's the track's machine
    for m in MACHINES:
        fn = "mb_%d.png" % m["id"]
        if not os.path.exists(os.path.join(SKIN, fn)):
            save_png("mb_%d" % m["id"], machine_bar(m))
        w_, h_ = Image.open(os.path.join(SKIN, fn)).size
        image_comp("Machine %s" % m["name"], fn, BAR_X, BAR_Y, w_, h_, t, cond=enabling(tk("machine"), m["id"], NMACH))

# LEV bar (Monomodule's LEVQ strips, in two segments) + its touch column
lev_x, lev_y = qx + (fx_ + 1) * S, qy + GRID_Y * S
inner_y0 = fy_ + 2
seg_n = 2
seg_h = (fh_l - 4) // seg_n
inner_h = seg_h * seg_n
lev_fns = []
for sgm in range(seg_n):
    frames = []
    for raw in range(FRAMES):
        cv = Canvas(17, seg_h)
        lvl = int(round(raw / 127.0 * inner_h))
        for r in range(seg_h):
            if inner_y0 + sgm * seg_h + r >= inner_y0 + inner_h - lvl:
                for c in range(1, 7):
                    cv.set(c, r)
        frames.append(cv.image())
    st = Image.new("RGB", (17 * S, seg_h * S * FRAMES))
    for i_, f_ in enumerate(frames):
        st.paste(f_, (0, i_ * seg_h * S))
    lev_fns.append(save_png("lev_%d" % sgm, st))
fn_t = save_png("touch_lev", Image.new("RGBA", (8, 8 * FRAMES), (0, 0, 0, 0)))
lev_tw, lev_th = TOUCH_W, 2 * CELL * S - 2 * TOUCH_INSET * S
for t in range(TRACKS):
    for sgm, fn in enumerate(lev_fns):
        place(knob_def(fn, 17 * S, seg_h * S, interactive=False), "LEV %d" % (sgm + 1), PIDX["track%d_level" % t],
              lev_x, lev_y + (inner_y0 + sgm * seg_h) * S, 17 * S, seg_h * S, t, img=fn, raw=100)
    place(knob_def(fn_t, lev_tw, lev_th), "LEV touch", PIDX["track%d_level" % t], qx + TOUCH_INSET * S,
          qy + (GRID_Y + TOUCH_INSET) * S, lev_tw, lev_th, t)

# machine picker: field over the machine bar toggles machine__open; panel + one image button per machine
pk_x, pk_y, pk_w, pk_h = OX + PAGES_X0, OY + TOP, PAGES_W, WIN_H - 8 - TOP
COL_ROWS = 16
cols = []
for m in MACHINES:
    col = next((c for c in cols if c["group"] == m["group"] and len(c["ms"]) < COL_ROWS), None)
    if not col:
        col = {"group": m["group"], "ms": []}
        cols.append(col)
    col["ms"].append(m)
cols.sort(key=lambda c: min(x["id"] for x in c["ms"]) if c["group"] != "ROM" else 1000 + min(x["id"] for x in c["ms"]))
COL_GAP, BORDER, HEADER_H, PAD, NAME_Y = 6, 3, 36, 6, 6
col_w = (pk_w - (len(cols) - 1) * COL_GAP) // len(cols)
ROW_H = (pk_h - 2 * BORDER - HEADER_H - 6) // COL_ROWS
panel = Image.new("RGB", (pk_w, pk_h), PAPER)
pd = ImageDraw.Draw(panel)


def dotted_h(dr, x0, x1, y):
    for x in range(x0, x1, 4):
        dr.rectangle([x, y, x + 1, y + 1], fill=INK)


rows = {}
x = 0
for col in cols:
    cx0 = x
    x += col_w + COL_GAP
    pd.rectangle([cx0, 0, cx0 + col_w - 1, pk_h - 1], outline=INK, width=BORDER)
    hx, hy, hw = cx0 + BORDER, BORDER, col_w - 2 * BORDER
    pd.rectangle([hx, hy, hx + hw - 1, hy + HEADER_H - 1], fill=INK)
    title = GROUP_TITLE.get(col["group"], col["group"])
    tw = text_width(F["bold8"], title) * S
    px_text(panel, F["bold8"], title, hx + hw // 2 - tw // 2, hy + HEADER_H // 2 - F["bold8"].h * S // 2, S, PAPER)
    ry = hy + HEADER_H + 6
    dotted_h(pd, hx, hx + hw, ry - 1)
    for i, m in enumerate(col["ms"]):
        rows[m["id"]] = (hx, ry + i * ROW_H, hw, ROW_H)
        dotted_h(pd, hx, hx + hw, ry + (i + 1) * ROW_H - 1)
fn_panel = save_png("pk_panel", panel)
pick_imgs = {}
for m in MACHINES:
    rx, ry, rw, rh = rows[m["id"]]
    imgs = {}
    for state in ("on", "off"):
        im = Image.new("RGB", (rw, rh), INK if state == "on" else PAPER)
        px_text(im, F["bold8"], m["short"], PAD, (rh - F["bold8"].h * 2) // 2, 2, PAPER if state == "on" else INK)
        imgs[state] = save_png("pk_%d_%s" % (m["id"], state), im)
    pick_imgs[m["id"]] = imgs
    key = "mdPickOpt_%d" % m["id"]
    defs[key] = ss._local(key, [ss._action("Mouse Down", "Q-Link")], [ss._button(imgs["on"], imgs["off"], m["id"], NMACH, rw, rh)])
pk = "mdPickPanel"
defs[pk] = ss._local(pk, [], [ss._sub("Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": fn_panel}, ss._bounds(0, 0, pk_w, pk_h), "Image")])
key = "mdPickField"
defs[key] = ss._local(key, [ss._action("Mouse Down", "Toggle Switch"), ss._action("Enter Pressed", "Toggle Switch")], [ss._focus(75 * S, BAR_ROWS * S)])
for t in range(TRACKS):   # last, so the picker draws over everything on the tab
    mk, ok = "track%d_machine" % t, "track%d_machine__open" % t
    place("mdPickField", "Machine picker", PIDX[ok], BAR_X, BAR_Y, 75 * S, BAR_ROWS * S, t, focus="Yes", extra={"Text": PIDX[mk]})
    open_c = enabling(ok, 1, 2)
    place(pk, "Machine list", PIDX[ok], pk_x, pk_y, pk_w, pk_h, t, cond=open_c, img=fn_panel)
    for m in MACHINES:
        rx, ry, rw, rh = rows[m["id"]]
        place("mdPickOpt_%d" % m["id"], "Machine %s" % m["name"], PIDX[mk], pk_x + rx, pk_y + ry, rw, rh, t, cond=open_c,
              img=pick_imgs[m["id"]]["on" if m["id"] == PREVIEW_MACHINE else "off"])

# ------------------------------------------------------------------ assemble ----------------------------------------
pages, qmap = [], []
comp_bg = {"version": 1, "colour": "ff%02x%02x%02x" % PAPER, "image": ""}
for t in range(TRACKS):
    tk = lambda k: "track%d_%s" % (t, k)
    sets = [("T%d SYN / EFX" % (t + 1), [tk("syn%d" % (k + 1)) for k in range(8)] + [tk(k) for k in AMP_KEYS]),
            ("T%d ROUTE / LEV" % (t + 1), [tk(k) for k in ROUTE_KEYS] + [tk("level"), tk("machine")])]
    comp = "MACHINEDRUM|TRACK %d" % (t + 1)
    for sp, (title, keys) in enumerate(sets):
        ql = {"Q-Link %d" % (q + 1): -1 for q in range(16)}
        for s_, k in enumerate(keys):
            ql["Q-Link %d" % ss.qlink_for_slot(s_)] = PIDX[k]
        pages.append({"version": 3, "tabName": "TRACK %d" % (t + 1), "fnKeyIndex": t, "fnKeySubIndex": sp, "qlinkBoundsData": ["0 0 0 0"],
                      "componentName": comp, "initialSize": "0 0 %d %d" % (SKIN_W, SKIN_H), "scale": 1.0})
        qmap.append({"Tab": t + 1, "SubTab": sp + 1, "Bank Direction": "Column", "Q-Links": ql})
    defs[comp] = {"key": comp, "value": {"version": 4, "actions": [], "backgroundData": {"version": 1, "focussed": comp_bg, "unfocussed": comp_bg},
                                         "ignoreMousePresses": False, "disableCoarseDataWheel": False, "repeats": 1,
                                         "hideQLinkBounds": True, "componentsData": TABK[t]}}
tui = {"pageData": {"version": 1, "componentDefinitions": {"version": 2, "importFiles": [ss.AKAI + "Generic/Generic Knob Overlay.json",
                                                                                        ss.AKAI + "Generic/Generic Menu Overlay.json"],
                                                          "localComponentDefinitions": list(defs.values())},
                    "info": {"version": 1, "type": "CompleteDescription"}, "tabs": pages}}
qlinks = {"version": 4, "info": {"version": 1, "type": "CompleteDescription"}, "Screen Mode Q-Links": {"version": 4, "map": qmap},
          "Program Mode Q-Links": dict(qmap[0]["Q-Links"])}
open(os.path.join(OUT, "version.xml"), "w").write(
    "<?xml version='1.0' encoding='utf-8'?>\n<plugincontent version=\"1.0\">\n\t<identifier>%s.vst.%s</identifier>\n"
    "\t<version>1.0.0.0</version>\n</plugincontent>\n" % (VENDOR, NAME.lower().replace(" ", "")))
for f, obj in (("TUI.json", tui), ("Q-Links.json", qlinks), ("Q-Links - 8by1.json", qlinks)):
    json.dump(obj, open(os.path.join(SKIN, f), "w"), indent=None if f == "TUI.json" else 1, separators=(",", ":") if f == "TUI.json" else None)
size = sum(os.path.getsize(os.path.join(SKIN, f)) for f in os.listdir(SKIN))
print("skin: %s (%d files, %.1f MB, %d components per tab)" % (OUT, len(os.listdir(SKIN)), size / 1e6, len(TABK[0])))


def preview(state, out, tab=0):
    """Composite of the skin for one state {param key: value}: what MPC would draw, at the given values."""
    im = Image.new("RGB", (SKIN_W, SKIN_H), PAPER)
    for fn, x, y, w, h, cond, raw, ptab in PREVIEW:
        if ptab is not None and ptab != tab:
            continue
        if cond:
            m = re.match(r"IndexedEnabling/(\d+)/(\d+)/Parameter (\d+)", cond)
            if state.get(params[int(m.group(3))]["key"], 0) != int(m.group(1)):
                continue
        src = Image.open(os.path.join(SKIN, fn)).convert("RGBA")
        if raw is not None:
            fh = src.size[1] // FRAMES
            src = src.crop((0, raw * fh, src.size[0], (raw + 1) * fh))
        im.paste(src, (x, y), src)
    im.save(out)


preview({"track0_machine": PREVIEW_MACHINE}, os.path.join(sys.argv[4], "preview_track1.png"), 0)
preview({"track1_machine": 20}, os.path.join(sys.argv[4], "preview_track2_rs.png"), 1)
preview({}, os.path.join(sys.argv[4], "preview_track3_new.png"), 2)
preview({"track0_machine": PREVIEW_MACHINE, "track0_machine__open": 1}, os.path.join(sys.argv[4], "preview_open.png"), 0)
