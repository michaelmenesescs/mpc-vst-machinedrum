#!/usr/bin/env python3
"""Builds the top-bar "machine bar" field - duplicating mpc-vst-monomodule's own `machine_bar()`
(vst/skin/mk_skin.py: a small pill showing [group logo or text] [machine name] [down-arrow], one baked
image per machine, shown via the same per-option IndexedEnabling trick as everything else keyed to
track%d_machine - see HANDOFF.md's "How the per-machine SYN labels actually become dynamic"). Per
user direction (2026-09-28): the picker becomes this top-bar field (tap it to open
build_machine_picker.py's panel), and the 4 tab quadrants stay pure displays of the real MD pages
(SYN/AMP-EFX/ROUTE + the picker's own space) rather than holding controls themselves.

Monomodule draws a real per-category logo when one exists, falling back to the group's short text
otherwise. The Machinedrum has no equivalent pixel-art category logos, so every bar always takes that
fallback path: category prefix text (TRX/ROM/MID/...), then the machine's own name, then the same
small down-arrow glyph (drawn procedurally - 3 decreasing-width rows, matching mk_skin.py's own loop)
to signal it's tappable.

usage: build_machine_bar.py <manifest.json (from build_machine_picker.py)> <label_font_atlas.json> <out dir>
"""
import json
import struct
import sys
import zlib
from pathlib import Path

BAR_H = 13   # one glyph row (5px) + 2px top/bottom padding + a little slack for the arrow
PAD = 3
GAP = 4
ARROW_W = 5


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)


def write_png(pix, w, h, path: Path) -> None:
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(0 if v else 255 for v in pix[y * w:(y + 1) * w])
    idat = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def text_width(text, font):
    return len(text) * font["glyph_w"]


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


def draw_arrow(canvas, cw, ch, x0, y0):
    """3 decreasing-width rows, pointing down - matches mk_skin.py's machine_bar() loop exactly."""
    for r in range(3):
        half = 2 - r
        for c in range(2 - half, 2 + half + 1):
            y = y0 + r
            x = x0 + c
            if 0 <= x < cw and 0 <= y < ch:
                canvas[y * cw + x] = 1


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    manifest = json.loads(Path(sys.argv[1]).read_text())
    font = json.loads(Path(sys.argv[2]).read_text())
    out_dir = Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)

    bar_manifest = {}
    for mid, info in manifest["machines"].items():
        name = info["name"]
        category = name[:3]
        parts_w = text_width(category, font) + GAP + text_width(name, font) + GAP + ARROW_W + PAD * 2
        w = parts_w
        canvas = bytearray(w * BAR_H)
        # solid ink background (a pill), matching Monomodule's cv.fill(...,True) - text/arrow drawn
        # in reverse (invert=True: 0 where ink would be) so it reads as light text on a dark pill.
        for i in range(len(canvas)):
            canvas[i] = 1
        x = PAD
        draw_text(canvas, w, BAR_H, x, (BAR_H - font["glyph_h"]) // 2, category, font, invert=True)
        x += text_width(category, font) + GAP
        draw_text(canvas, w, BAR_H, x, (BAR_H - font["glyph_h"]) // 2, name, font, invert=True)
        x += text_width(name, font) + GAP
        # the arrow is drawn "on" (0, dark) over the light pill - flip its polarity vs. the inverted text
        for r in range(3):
            half = 2 - r
            for c in range(2 - half, 2 + half + 1):
                y = (BAR_H // 2 - 1) + r
                xx = x + c
                if 0 <= xx < w and 0 <= y < BAR_H:
                    canvas[y * w + xx] = 0

        fn = f"bar_{mid}.png"
        write_png(canvas, w, BAR_H, out_dir / fn)
        bar_manifest[mid] = {"name": name, "file": fn, "w": w, "h": BAR_H}

    (out_dir / "bar_manifest.json").write_text(json.dumps(bar_manifest, indent=1))
    print(f"{len(bar_manifest)} machine bar images written to {out_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
