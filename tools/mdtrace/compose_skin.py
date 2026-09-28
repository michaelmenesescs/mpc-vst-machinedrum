#!/usr/bin/env python3
"""Phase 4 of the "Monomodule way" skin plan (HANDOFF.md): the compositor. Given the font atlas
(build_font_atlas.py), a dial-pointer atlas per knob (build_dial_atlas.py), and the hand-authored
layout (ui_spec.py), draws one screen's dial-grid region - labels + pointer icons - for a chosen set
of live values, the same job Monomodule's own mk_skin.py does with its ROM-decoded assets.

This is a first working version, proving the three atlases compose into a recognizable screen. Not
yet wired into vst/gen_layout.py's actual skin build (that's the next step once this is visually
confirmed against a real capture) and doesn't draw any of the surrounding chrome (KIT name box, LEV
meter - parked, breadcrumb) - just the dial-grid region this session's atlases actually cover.

usage: compose_skin.py <font_atlas.json> <dial_atlas_dir with DataEntryA..H subdirs> <screen: amp_fx|syn>
                        <label1,label2,...> <value1,value2,...> <out.png>
  dial_atlas_dir/DataEntryX/dial_atlas.json is expected for X in A..H (matches how this session's
  build_dial_atlas.py runs were laid out - see HANDOFF.md).
"""
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ui_spec import DIAL_GRID  # noqa: E402

CANVAS_W, CANVAS_H = 80, 64  # LCD x=48..128, the dial-grid region only (see module docstring)
ORIGIN_X = 48  # ui_spec's dial_x0/label coords are in full-LCD (0..128) space; canvas starts at x=48


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)


def write_png(bits, w, h, path: Path, scale=1) -> None:
    raw = bytearray()
    for y in range(h * scale):
        raw.append(0)
        sy = y // scale
        for x in range(w * scale):
            sx = x // scale
            raw.append(0 if bits[sy * w + sx] else 255)
    idat = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(">IIBBBBB", w * scale, h * scale, 8, 0, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def blit(canvas, cw, ch, x0, y0, rows):
    """rows: list of '01..' strings. Clips to canvas bounds; out-of-range cells are skipped."""
    for y, row in enumerate(rows):
        cy = y0 + y
        if not (0 <= cy < ch):
            continue
        for x, bit in enumerate(row):
            cx = x0 + x
            if 0 <= cx < cw and bit == "1":
                canvas[cy * cw + cx] = 1


def draw_text(canvas, cw, ch, x0, y0, text, font):
    x = x0
    for label_char in text:
        key = {
            " ": "SPACE", "+": "PLUS", "-": "MINUS", "=": "EQUALS", "/": "SLASH",
            "(": "LPAREN", ")": "RPAREN", ",": "COMMA", "!": "BANG", "?": "QUESTION",
        }.get(label_char, label_char.upper())
        glyph = font["glyphs"].get(key)
        if glyph is None:
            x += 4
            continue
        blit(canvas, cw, ch, x, y0, glyph["rows"])
        x += glyph["advance"] + 1  # +1px inter-glyph gap; not measured, a reasonable first guess


def dial_state_for(dial_atlas, value):
    for st in dial_atlas["states"]:
        if st["value_first"] <= value <= st["value_last"]:
            return st
    return dial_atlas["states"][-1]


def main() -> int:
    if len(sys.argv) < 7:
        print(__doc__, file=sys.stderr)
        return 2
    font = json.loads(Path(sys.argv[1]).read_text())
    dial_dir = Path(sys.argv[2])
    screen = sys.argv[3]
    labels = sys.argv[4].split(",")
    values = [int(v) for v in sys.argv[5].split(",")]
    out_path = Path(sys.argv[6])

    grid = DIAL_GRID
    assert len(labels) == grid["cols"] * grid["rows"] == len(values)

    canvas = bytearray(CANVAS_W * CANVAS_H)
    encoder_names = ["DataEntryA", "DataEntryB", "DataEntryC", "DataEntryD",
                     "DataEntryE", "DataEntryF", "DataEntryG", "DataEntryH"]

    for i, (label, value) in enumerate(zip(labels, values)):
        col, row = i % grid["cols"], i // grid["cols"]
        dial_atlas = json.loads((dial_dir / encoder_names[i] / "dial_atlas.json").read_text())
        state = dial_state_for(dial_atlas, value)
        dx = grid["dial_x0"] + col * grid["dial_col_pitch"] - ORIGIN_X
        dy = grid["dial_y0"] + row * grid["dial_row_pitch"]
        blit(canvas, CANVAS_W, CANVAS_H, dx, dy, state["rows"])

        lx = grid["dial_x0"] + col * grid["label_col_pitch"] - ORIGIN_X
        ly = grid["label_y0"] + row * grid["dial_row_pitch"]
        draw_text(canvas, CANVAS_W, CANVAS_H, lx, ly, label, font)

    write_png(canvas, CANVAS_W, CANVAS_H, out_path, scale=8)
    print(f"wrote {out_path} ({screen}, values={values})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
