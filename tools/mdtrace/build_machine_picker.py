#!/usr/bin/env python3
"""Builds a Machinedrum machine picker, duplicating mpc-vst-monomodule's own hand-built picker design
(vst/skin/mk_skin.py: one column per machine category, all rows visible with no scrolling, a dotted
separator between rows, one on/off toggle image per machine) rather than the generic layout `popup`
widget, which can't paginate and would be unusable for 135 machines (see HANDOFF.md's "Skin plan: the
Monomodule way", machine picker discussion, 2026-09-28).

Categories are the machine name's first 3 characters (ROM, MID, E12, TRX, P-I, RAM, EFM, INP, CTR,
GND - found by grouping mdmachine's real listing, not guessed), each its own column. Unlike
Monomodule's version (which fits a much shorter machine list with room for per-machine description
text), there's no room for blurbs here: the largest category (ROM, 48 machines) needs every row
squeezed to 12px tall just to fit the full list on one screen without scrolling, matching Monomodule's
"no scroll" design rather than its exact row size.

This generates the PANEL BACKGROUND (column borders, headers, dotted row separators) and one on/off
row image per machine, plus a manifest for wiring into gen_layout.py next (not done yet - this is the
asset-generation half only, see HANDOFF.md for the remaining IndexedEnabling wiring).

usage: build_machine_picker.py <mdmachine binary> <OS.syx> <label_font_atlas.json> <out dir>
"""
import json
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

PANEL_W, PANEL_H = 1280, 600
COL_GAP = 6
ROW_H = 12
HEADER_H = 20
PAD = 4


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)


def new_canvas(w, h, fill=0):
    return bytearray([fill]) * (w * h)


def write_png(pix, w, h, path: Path) -> None:
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(0 if v else 255 for v in pix[y * w:(y + 1) * w])
    idat = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def hline(canvas, cw, x0, x1, y, dotted=False):
    for x in range(x0, x1, 2 if dotted else 1):
        if 0 <= y < 10 ** 9:
            canvas[y * cw + x] = 1


def rect_border(canvas, cw, ch, x0, y0, w, h):
    for x in range(x0, x0 + w):
        canvas[y0 * cw + x] = 1
        canvas[(y0 + h - 1) * cw + x] = 1
    for y in range(y0, y0 + h):
        canvas[y * cw + x0] = 1
        canvas[y * cw + x0 + w - 1] = 1


def fill_rect(canvas, cw, x0, y0, w, h, v=1):
    for y in range(y0, y0 + h):
        row0 = y * cw
        for x in range(x0, x0 + w):
            canvas[row0 + x] = v


def draw_text(canvas, cw, ch, x0, y0, text, font, invert=False):
    pitch = font["glyph_w"]
    x = x0
    for c in text:
        rows = font["glyphs"].get(c.upper())
        if rows is not None:
            for y, row in enumerate(rows):
                cy = y0 + y
                if not (0 <= cy < ch):
                    continue
                for xi, bit in enumerate(row):
                    cx = x + xi
                    if 0 <= cx < cw and bit == "1":
                        canvas[cy * cw + cx] = 0 if invert else 1
        x += pitch


def parse_machines(mdmachine_out: str):
    machines = []
    for line in mdmachine_out.splitlines():
        m = re.match(r"\s*(\d+)\s+(\S+)\s+fn=\$[0-9a-f]+\s+(.*)$", line)
        if not m:
            continue
        mid, name, rest = m.groups()
        if int(mid) < 16:
            continue  # ids 0-3 are internal utility machines (GND--/GNDSN/GNDNS/GNDIM), not real voices
        machines.append((int(mid), name, (rest.split() + [""] * 8)[:8]))
    return machines


def main() -> int:
    if len(sys.argv) < 5:
        print(__doc__, file=sys.stderr)
        return 2
    mdmachine, syx, font_path, out_dir = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
    out_dir.mkdir(parents=True, exist_ok=True)
    font = json.loads(font_path.read_text())

    result = subprocess.run([mdmachine, syx], capture_output=True, text=True, check=True)
    machines = parse_machines(result.stdout)

    groups = []
    by_prefix = {}
    for mid, name, params in machines:
        prefix = name[:3]
        if prefix not in by_prefix:
            by_prefix[prefix] = {"prefix": prefix, "machines": []}
            groups.append(by_prefix[prefix])
        by_prefix[prefix]["machines"].append((mid, name, params))
    print(f"{len(machines)} machines in {len(groups)} categories: "
          f"{', '.join('%s(%d)' % (g['prefix'], len(g['machines'])) for g in groups)}", file=sys.stderr)

    col_w = (PANEL_W - (len(groups) - 1) * COL_GAP) // len(groups)
    panel = new_canvas(PANEL_W, PANEL_H)
    manifest = {}

    x = 0
    for g in groups:
        rect_border(panel, PANEL_W, PANEL_H, x, 0, col_w, PANEL_H)
        fill_rect(panel, PANEL_W, x + 1, 1, col_w - 2, HEADER_H - 2, v=1)
        draw_text(panel, PANEL_W, PANEL_H, x + PAD, 6, g["prefix"], font, invert=True)
        ry = HEADER_H + 2
        for i, (mid, name, params) in enumerate(g["machines"]):
            row_y = ry + i * ROW_H
            if row_y + ROW_H > PANEL_H - 1:
                print(f"warning: {g['prefix']} row {i} ({name}) doesn't fit "
                      f"({row_y + ROW_H} > {PANEL_H}) - increase PANEL_H or shrink ROW_H", file=sys.stderr)
                continue
            hline(panel, PANEL_W, x + 1, x + col_w - 1, row_y + ROW_H - 1, dotted=True)
            draw_text(panel, PANEL_W, PANEL_H, x + PAD, row_y + 2, name, font)
            manifest[mid] = {"name": name, "params": params,
                              "rect": [x + 2, row_y, col_w - 4, ROW_H - 1]}
        x += col_w + COL_GAP

    write_png(panel, PANEL_W, PANEL_H, out_dir / "picker_panel.png")
    (out_dir / "manifest.json").write_text(json.dumps({
        "panel_w": PANEL_W, "panel_h": PANEL_H, "machines": manifest,
    }, indent=1))
    print(f"wrote {out_dir / 'picker_panel.png'} and manifest.json ({len(manifest)} machines placed)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
