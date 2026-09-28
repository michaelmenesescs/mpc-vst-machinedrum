#!/usr/bin/env python3
"""Writes layout.conf for Machinedrum One: one tab per track, confirmed layout (HANDOFF.md, "Tab
layout confirmed with the user against a real Monomodule VST screenshot"):

  - A MACHINE control at top-left (a plain knob for now - the real Monomodule-style picker pill,
    tools/mdtrace/build_machine_bar.py + build_machine_picker.py, isn't wired into layout.conf yet;
    see HANDOFF.md for what's still needed there).
  - A 2x2 grid of the Machinedrum's own real per-track pages, each its own native `frame title=`
    (baked by shadow_skin.py itself, real proportional text, no custom font atlas needed for these
    fixed labels - see HANDOFF.md): SYN (top-left, no knobs yet - SYN1-8 aren't exposed as params,
    blocked on "touched" tracking in engine.cpp), AMP/EFX (top-right, 8 knobs), ROUTE (bottom-left,
    8 knobs: DIST VOL PAN DEL / REV LFOS LFOD LFOM - matches real hardware's own ROUTE page grouping,
    so VOL/PAN live here now instead of a separate top-level knob), and one reserved/blank quadrant
    (bottom-right - confirmed with the user: leave it, don't invent content for it).

AMP/EFX's 8 knobs and ROUTE's 6 not-otherwise-elsewhere knobs (DIST/DEL/REV/LFOS/LFOD/LFOM) use real
bit-exact dial-pointer filmstrips (`strip=`/`frames=128`) at `build/knobs/<key>.png` - run
tools/mdtrace/build_amp_fx_knob_strips.py and build_route_knob_strips.py before this script. VOL/PAN
and MACHINE keep the generic default knob look (no per-value icon captured for those - VOL/PAN look
like a standard fader-style dial on real hardware anyway, not a bounded icon set)."""

FX_PARAMS = [("amd", "AMD"), ("amf", "AMF"), ("eqf", "EQF"), ("eqg", "EQG"),
             ("fltf", "FLTF"), ("fltw", "FLTW"), ("fltq", "FLTQ"), ("srr", "SRR")]
ROUTE_PARAMS = [("dist", "DIST"), ("vol", "VOL"), ("pan", "PAN"), ("del", "DEL"),
                ("rev", "REV"), ("lfos", "LFOS"), ("lfod", "LFOD"), ("lfom", "LFOM")]
ROUTE_STRIP_KEYS = {"dist", "del", "rev", "lfos", "lfod", "lfom"}  # vol/pan have no captured filmstrip
FX_STRIP_KEYS = {k for k, _ in FX_PARAMS}  # all 8 AMP/EFX params have captured filmstrips

lines = [
    "# Machinedrum One skin layout: one tab per track, a MACHINE knob (picker not wired yet) and a",
    "# 2x2 grid of the Machinedrum's own real per-track pages (SYN/AMP-EFX/ROUTE + one reserved) -",
    "# see HANDOFF.md, \"Tab layout confirmed with the user against a real Monomodule VST screenshot\".",
    "style=td3",
    "theme_bg=17171a",
    "theme_panel=201f1e",
    "theme_line=3a3936",
    "theme_ink=d8d6ce",
    "theme_ink_dim=8a8880",
    "theme_accent=d9861f",
    "theme_accent_hi=f2a83c",
    "theme_knob_face=1c1c1c",
    "theme_knob_ring=3a3a3a",
    "theme_knob_dot=d9861f",
    "theme_lcd=141311",
    "theme_box=201f1e",
    "theme_btn_bg=121212",
    "theme_btn_text=f2f0e8",
    "",
]

# 2x2 grid geometry (plugin area is 1280x628 - shadow_skin.py's own coordinate doc).
GRID_X0, GRID_Y0 = 20, 130
CELL_W, CELL_H, GAP = 610, 240, 20
QUAD_XY = {
    "syn": (GRID_X0, GRID_Y0),
    "amp_fx": (GRID_X0 + CELL_W + GAP, GRID_Y0),
    "route": (GRID_X0, GRID_Y0 + CELL_H + GAP),
    "reserved": (GRID_X0 + CELL_W + GAP, GRID_Y0 + CELL_H + GAP),
}
# knob positions within a quadrant (relative to its x0,y0), 4 cols x 2 rows
KNOB_COLS = [90, 240, 390, 540]
KNOB_ROWS = [110, 195]
KNOB_R = 26


def quadrant(qx, qy, title):
    return ['frame x=%d y=%d w=%d h=%d title="%s"' % (qx, qy, CELL_W, CELL_H, title)]


def knob_grid(qx, qy, params, key_fmt, t, strip_keys=frozenset()):
    out, keys = [], []
    for i, (key, label) in enumerate(params):
        cx, cy = qx + KNOB_COLS[i % 4], qy + KNOB_ROWS[i // 4]
        pkey = key_fmt % (t, key)
        strip = " strip=build/knobs/%s.png frames=128" % key if key in strip_keys else ""
        out.append('knob cx=%d cy=%d r=%d label="%s" key=%s%s' % (cx, cy, KNOB_R, label, pkey, strip))
        keys.append(pkey)
    return out, keys

for t in range(16):
    tab = "TRACK %d" % (t + 1)
    lines.append("[tab %s]" % tab)

    mx, my = GRID_X0 + 60, 60
    lines.append('knob cx=%d cy=%d r=36 label="MACHINE" key=track%d_machine' % (mx, my, t))
    keys = ["track%d_machine" % t]

    lines += quadrant(*QUAD_XY["syn"], "SYN")
    # SYN1-8 aren't exposed as params yet (blocked on "touched" tracking, engine.cpp) - frame only.

    lines += quadrant(*QUAD_XY["amp_fx"], "AMP / EFX")
    knobs, ks = knob_grid(*QUAD_XY["amp_fx"], FX_PARAMS, "track%d_%s", t, strip_keys=FX_STRIP_KEYS)
    lines += knobs
    keys += ks

    lines += quadrant(*QUAD_XY["route"], "ROUTE")
    knobs, ks = knob_grid(*QUAD_XY["route"], ROUTE_PARAMS, "track%d_%s", t, strip_keys=ROUTE_STRIP_KEYS)
    lines += knobs
    keys += ks

    lines += quadrant(*QUAD_XY["reserved"], "-")
    # Reserved - confirmed with the user (HANDOFF.md): leave blank, no invented content.

    lines.append('qlinks "%s" = %s' % (tab, ",".join(keys)))
    lines.append("")

lines.append("[tab GLOBAL]")
lines.append('knob cx=200 cy=300 r=40 label="TEMPO" key=tempo')
lines.append('knob cx=400 cy=300 r=40 label="MAX VOICES" key=max_voices')
lines.append('qlinks "GLOBAL" = tempo,max_voices')

open("layout.conf", "w").write("\n".join(lines) + "\n")
print("layout.conf: %d tabs" % 17)
