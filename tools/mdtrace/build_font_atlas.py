#!/usr/bin/env python3
"""Builds a font glyph atlas (JSON, build output only - never committed, see docs/FIRMWARE.md) from
the Machinedrum's own LCD, for the Phase 1 "Monomodule way" skin (HANDOFF.md, "Skin plan: the
Monomodule way"). There's no ROM font decoder in this project (unlike Monomodule's RomArt.cpp), so
instead this drives the front panel's kit-name character-picker screen through its full character
cycle and slices each frame's changed glyph cell out by pixel-diffing across frames - the cell that
changes between frames *is* the glyph, no ROM decoding needed.

Mechanism (confirmed empirically 2026-09-28, see HANDOFF.md Phase 1 section for the discovery log):
  panel:Kit panel:Right panel:Enter panel:Enter   -> reaches a "1-T RX UW"-style "Enter name:" screen,
                                                      cursor on the name's first character
  panel:Up:64 / panel:Down:64                     -> steps the cursor's character forward/back by
                                                      exactly one in a fixed 50-entry cycle (confirmed
                                                      by walking the full cycle and reading it back)
  panel:Right:64 / panel:Left:64                  -> moves the cursor to the next/previous name
                                                      position (not yet swept per-position - this
                                                      script only builds position 0's glyph cell)

CYCLE is the 50-entry character order read off the walk, starting from a space. Two entries (index 14
and 49) both look like "the same glyph" on inspection (see HANDOFF.md) - kept as distinct cycle slots
here rather than guessed-at Unicode, since we don't need semantic correctness for them, just a correct
bitmap keyed by a stable label.

This is a proportional (not fixed-pitch) font: a cursor-position sweep showed the name-entry screen's
selection box resizing to each glyph's width rather than moving in constant steps. So each glyph's
JSON entry carries its own ink-trimmed `advance` width alongside the bitmap - a compositor lays text
out by summing advances, not by multiplying a cell width.

usage: build_font_atlas.py <mdProbe binary> <flash.bin> <out dir>
Writes <out dir>/font_atlas.json and a <out dir>/font_atlas_grid.png for visual sanity-checking.
"""
import json
import subprocess
import struct
import sys
import zlib
from pathlib import Path

CYCLE = [
    "SPACE", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "PLUS", "MINUS", "EQUALS", "SPECIAL_OSLASH_A", "SLASH", "LPAREN", "RPAREN", "COMMA", "BANG", "QUESTION",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", "ARING", "ADIAERESIS", "SPECIAL_OSLASH_B",
]
assert len(CYCLE) == 50

# Autocrop band for the changing glyph at name-position 0 (pixels, in the 128x64 LCD) - found by
# scanning for columns/rows whose pixel pattern actually varies across the 50 captured frames.
# Re-derive by hand (see HANDOFF.md's discovery log) if the firmware/layout ever changes.
GLYPH_BAND_X = (17, 24)
GLYPH_BAND_Y = (31, 44)
# Within GLYPH_BAND_Y, rows 0-2 and 11-12 (0-based within the 13-row band) hold the cursor box's own
# dashed corner ticks, not glyph body - found by intersecting all 50 captured glyphs' bitmaps (see
# HANDOFF.md). Row range is a half-open [start, end).
GLYPH_BODY_ROWS = (3, 11)


def load_ppm(path: Path):
    data = path.read_bytes()
    assert data[:2] == b"P5", "not a binary PGM"
    i = 2

    def skip_ws(i):
        while data[i:i + 1].isspace():
            i += 1
        return i

    def token(i):
        i = skip_ws(i)
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        return data[i:j], j

    w, i = token(i)
    h, i = token(i)
    _mx, i = token(i)
    i += 1
    w, h = int(w), int(h)
    return w, h, data[i:i + w * h]


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)


def write_png(pix: bytes, w: int, h: int, path: Path) -> None:
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(pix[y * w:(y + 1) * w])
    idat = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: build_font_atlas.py <mdProbe binary> <flash.bin> <out dir>", file=sys.stderr)
        return 2
    mdprobe, flash, out_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    flashcache = out_dir / "_flashcache.bin"

    ppm_dir = out_dir / "_font_frames"
    ppm_dir.mkdir(exist_ok=True)

    args = [str(mdprobe), str(flash), str(flashcache),
            "panel:Kit", "panel:Right", "panel:Enter", "panel:Enter"]
    # The default kit name's first character is "T" = CYCLE index 40. Up moves forward through CYCLE,
    # so reaching SPACE (index 0) from T needs (0 - 40) % 50 = 10 taps, NOT `CYCLE.index("T")` taps
    # (that earlier version landed 30 slots short of SPACE, silently mislabeling every glyph 10 slots
    # off - e.g. "A"'s stored bitmap was actually PLUS's. Caught by rendering ASCII dumps of specific
    # letters and checking their shape, not just eyeballing a relabeled grid - a relabeled-then-
    # redisplayed grid looks self-consistently "right" even when every key points at the wrong glyph).
    # After this preloop we're exactly at SPACE (index 0), so frame i (0-based) needs no `start_index`
    # term in its label lookup below - it shows CYCLE[(i + 1) % 50].
    start_index = CYCLE.index("T")
    for _ in range((-start_index) % 50):
        args.append("panel:Up:64")
    frame_paths = []
    for step in range(1, 51):
        p = ppm_dir / f"f{step}.ppm"
        args += ["panel:Up:64", f"lcdpng:{p}"]
        frame_paths.append(p)

    print("driving mdProbe through the full 50-character cycle...", file=sys.stderr)
    subprocess.run(args, check=True, capture_output=True)

    w = h = None
    frames = []
    for p in frame_paths:
        fw, fh, pix = load_ppm(p)
        w, h = fw, fh
        frames.append(pix)

    x0, x1 = GLYPH_BAND_X
    y0, y1 = GLYPH_BAND_Y
    cw, ch = x1 - x0, y1 - y0

    atlas = {}
    grid_cols, grid_gap = 10, 2
    grid_rows = (len(frames) + grid_cols - 1) // grid_cols
    gw = grid_cols * cw + (grid_cols + 1) * grid_gap
    gh = grid_rows * ch + (grid_rows + 1) * grid_gap
    grid = bytearray([255]) * (gw * gh)

    for i, pix in enumerate(frames):
        label = CYCLE[(i + 1) % 50]
        cell_rows = []
        cell = bytearray()
        for y in range(y0, y1):
            row_bits = "".join("1" if pix[y * w + x] < 128 else "0" for x in range(x0, x1))
            cell_rows.append(row_bits)
            cell.extend(1 if b == "1" else 0 for b in row_bits)
        # last write wins if a label repeats (SPECIAL_OSLASH_A/_B are intentionally distinct keys, so
        # this only fires if CYCLE has an actual duplicate - keep first occurrence instead)
        if label not in atlas:
            # The name-entry screen's cursor box resizes to hug each glyph (confirmed by diffing a
            # cursor-position sweep - deltas of 5-9px between positions, not a constant pitch), so
            # this is a proportional font, not fixed-pitch as originally assumed in the Phase 1 plan.
            # The box draws dashed corner ticks in this crop band's rows 0-2 and 11-12 (confirmed by
            # intersecting all 50 glyphs' bitmaps: only col0-1 of rows 1 and 12 are ink in literally
            # every glyph, and the *other* corner - which moves - lands in those same row bands too),
            # so ink_x0/ink_x1/advance are measured only over GLYPH_BODY_ROWS, which every sample glyph
            # confirmed holds the actual character body with no border bleed.
            body = range(*GLYPH_BODY_ROWS)
            ink_cols = [x for x in range(cw) if any(cell_rows[y][x] == "1" for y in body)]
            ink_x0, ink_x1 = (ink_cols[0], ink_cols[-1] + 1) if ink_cols else (0, 0)
            atlas[label] = {
                "w": cw, "h": ch, "rows": cell_rows,
                "ink_x0": ink_x0, "ink_x1": ink_x1, "advance": ink_x1 - ink_x0 + 1,
            }
        r, c = divmod(i, grid_cols)
        gx0 = grid_gap + c * (cw + grid_gap)
        gy0 = grid_gap + r * (ch + grid_gap)
        for y in range(ch):
            for x in range(cw):
                grid[(gy0 + y) * gw + (gx0 + x)] = 0 if cell[y * cw + x] else 255

    (out_dir / "font_atlas.json").write_text(json.dumps({
        "glyph_w": cw, "glyph_h": ch,
        "band_x": list(GLYPH_BAND_X), "band_y": list(GLYPH_BAND_Y),
        "glyphs": atlas,
    }, indent=1))
    write_png(bytes(grid), gw, gh, out_dir / "font_atlas_grid.png")

    for p in frame_paths:
        p.unlink()
    ppm_dir.rmdir()

    print(f"{len(atlas)} glyphs written to {out_dir / 'font_atlas.json'}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
