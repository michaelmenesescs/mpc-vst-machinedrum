#!/usr/bin/env python3
"""Extracts the SMALL header-label font (distinct from build_font_atlas.py's name-entry font - see
HANDOFF.md, Phase 4: the label band is 5px tall vs. the name-entry font's 13px, a real font-size
mismatch found by trying to compose a skin and seeing labels overlap). This is Phase 4's follow-up
extraction pass, using a variant of the same idea: instead of one glyph slot cycling through a known
character order (there's no "rename" screen for this font), sweep the SYN page's machine assignment
(`PanelEncoder::SoundSelection`) so the label text itself varies with the machine (Phase 3 already
established this comes from `MachineInfo::params`), read off the resulting words by eye once (they're
short, 3-4 capital letters, easy to OCR from an upscaled capture - see WORDS_BY_FRAME below, hand-
transcribed from a 26-frame sweep this session), then auto-slice each word into per-letter cells at
this font's measured 4px pitch (confirmed by comparing the constant word "DEC", which appears in every
frame regardless of machine, and "PTCH": both show letters starting exactly 4px apart).

This gets ~20 of 26 letters "for free" from one sweep. It's not exhaustive (no J/K/Q/X/Z in this
particular word set) - extend WORDS_BY_FRAME with a longer sweep (or a second one starting from a
different machine) to cover the rest before Phase 4 needs to render arbitrary machine names.

usage: build_label_font_atlas.py <mdProbe binary> <flash.bin> <out dir>
"""
import json
import subprocess
import struct
import sys
import zlib
from pathlib import Path

# Hand-transcribed from a 26-frame SoundSelection sweep this session (see HANDOFF.md Phase 4 section)
# by reading a stacked strip of the label row upscaled 6x. Index = sweep step (0 = no turns yet).
WORDS_BY_FRAME = {
    0: ["PTCH", "DEC", "RAMP", "HOLD"], 3: ["PTCH", "DEC", "BUMP", "BENV"],
    5: ["PTCH", "DEC", "RAMP", "RDEC"], 9: ["CLPY", "TONE", "RAMP", "RDEC"],
    10: ["CLPY", "TONE", "HARD", "RICH"], 13: ["PTCH", "DEC", "DIST", None],
    15: ["PTCH", "DEC", "ENH", "DAMP"], 16: ["GAP", "DEC", "HPF", "LPF"],
    19: ["RICH", "DEC", "TOP", "TTUN"], 22: ["PTCH", "DEC", "HOLD", "BRR"],
}
MAX_FRAME = max(WORDS_BY_FRAME)

# The bottom breadcrumb ("TRX*XC*SYNT" - "*" is a placeholder for the "*" arrow glyph between
# segments, kept so the pitch-4 slicer stays aligned; it's never extracted) is the SAME small font,
# used here to pick up letters (X, K) this session's SYN-machine word sample never happened to hit.
# Found by inspecting the same 26-frame sweep used for WORDS_BY_FRAME, at these two extra steps.
BREADCRUMB_Y = (57, 62)
BREADCRUMB_WORDS_BY_FRAME = {4: "TRX*XC*SYNT", 20: "ROM14*ATAK*S"}

LABEL_Y = (3, 8)          # half-open row range within the LCD (see module docstring)
COL_X0 = [48, 68, 88, 108]  # column left edges (same pitch as DIAL_GRID's label_col_pitch)
COL_W = 20
GLYPH_PITCH = 4            # px per character, confirmed by comparing "DEC"/"PTCH" letter starts
GLYPH_H = LABEL_Y[1] - LABEL_Y[0]


def load_ppm(path: Path):
    data = path.read_bytes()
    assert data[:2] == b"P5"
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


def slice_word_at(pix, w, col_x0, word, y_range):
    """Auto-finds the word's first ink column within its column box, then slices at GLYPH_PITCH."""
    y0, y1 = y_range
    ink_cols = [x for x in range(col_x0, col_x0 + COL_W)
                if any(pix[y * w + x] < 128 for y in range(y0, y1))]
    if not ink_cols:
        return {}
    start = ink_cols[0]
    glyphs = {}
    for i, letter in enumerate(word):
        gx0 = start + i * GLYPH_PITCH
        rows = ["".join("1" if pix[y * w + x] < 128 else "0" for x in range(gx0, gx0 + GLYPH_PITCH))
                for y in range(y0, y1)]
        glyphs[letter] = rows
    return glyphs


def slice_word(pix, w, col_x0, word):
    return slice_word_at(pix, w, col_x0, word, LABEL_Y)


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: build_label_font_atlas.py <mdProbe binary> <flash.bin> <out dir>", file=sys.stderr)
        return 2
    mdprobe, flash, out_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    flashcache = out_dir / "_flashcache.bin"
    ppm_dir = out_dir / "_label_frames"
    ppm_dir.mkdir(exist_ok=True)

    args = [str(mdprobe), str(flash), str(flashcache)]
    frame_paths = []
    breadcrumb_paths = []
    for step in range(0, MAX_FRAME + 1):
        if step in WORDS_BY_FRAME:
            p = ppm_dir / f"f{step}.ppm"
            args.append(f"lcdpng:{p}")
            frame_paths.append((step, WORDS_BY_FRAME[step], p))
        if step in BREADCRUMB_WORDS_BY_FRAME:
            bp = ppm_dir / f"bc{step}.ppm"
            args.append(f"lcdpng:{bp}")
            breadcrumb_paths.append((BREADCRUMB_WORDS_BY_FRAME[step], bp))
        if step < MAX_FRAME:
            args.append("encoder:SoundSelection:1")

    print(f"driving mdProbe through {len(WORDS_BY_FRAME)} known machine assignments...", file=sys.stderr)
    subprocess.run(args, check=True, capture_output=True)

    # The AMP/EFX page's own fixed labels (AMD AMF EQF EQG / FLTF FLTW FLTQ SRR) are ground truth for
    # free, and cover Q/W - the only two letters the SYN-machine sweep above didn't happen to hit.
    # Captured as a SEPARATE fresh-boot run, not appended after the sweep above: doing it in the same
    # process left the display in a subtly shifted state (confirmed by comparing pixel data - letters
    # came out garbled/misaligned when captured right after 22 encoder turns, but read correctly from
    # a clean boot) - some lingering UI state from the machine-select sweep bleeds into this capture.
    amp_fx_words = [["AMD", "AMF", "EQF", "EQG"], ["FLTF", "FLTW", "FLTQ", "SRR"]]
    amp_fx_path = ppm_dir / "amp_fx.ppm"
    subprocess.run([str(mdprobe), str(flash), str(flashcache),
                    "panel:SynthesisEffectsRouting", f"lcdpng:{amp_fx_path}"],
                   check=True, capture_output=True)
    frame_paths.append(("amp_fx_row0", amp_fx_words[0], amp_fx_path))
    # row 1 (FLTF..SRR) is a separate y-band (LABEL_Y shifted by the grid's row pitch) - handled below.

    atlas = {}
    for step, words, p in frame_paths:
        w, h, pix = load_ppm(p)
        for col_x0, word in zip(COL_X0, words):
            if not word:
                continue
            for letter, rows in slice_word(pix, w, col_x0, word).items():
                atlas.setdefault(letter, rows)  # first occurrence wins
        if step == "amp_fx_row0":
            row1_y = (LABEL_Y[0] + 31, LABEL_Y[1] + 31)  # DIAL_GRID's dial_row_pitch, see ui_spec.py
            for col_x0, word in zip(COL_X0, amp_fx_words[1]):
                for letter, rows in slice_word_at(pix, w, col_x0, word, row1_y).items():
                    atlas.setdefault(letter, rows)

    # The breadcrumb is a bright bar (white text on black), the opposite polarity of every other
    # capture in this script - invert before reusing the same slicer.
    for word, p in breadcrumb_paths:
        w, h, pix = load_ppm(p)
        inv = bytearray(255 - b for b in pix)
        for letter, rows in slice_word_at(inv, w, 0, word, BREADCRUMB_Y).items():
            if letter != "*":
                atlas.setdefault(letter, rows)

    print(f"{len(atlas)} letters extracted: {''.join(sorted(atlas))}", file=sys.stderr)

    cols, gap = 10, 2
    rows_n = (len(atlas) + cols - 1) // cols
    gw = cols * GLYPH_PITCH + (cols + 1) * gap
    gh = rows_n * GLYPH_H + (rows_n + 1) * gap
    grid = bytearray(gw * gh)  # 0 = background; write_png below treats any nonzero as ink
    for i, (letter, rows) in enumerate(sorted(atlas.items())):
        r, c = divmod(i, cols)
        gx0 = gap + c * (GLYPH_PITCH + gap)
        gy0 = gap + r * (GLYPH_H + gap)
        for y in range(GLYPH_H):
            for x in range(GLYPH_PITCH):
                grid[(gy0 + y) * gw + (gx0 + x)] = 1 if rows[y][x] == "1" else 0

    (out_dir / "label_font_atlas.json").write_text(json.dumps({
        "glyph_w": GLYPH_PITCH, "glyph_h": GLYPH_H, "glyphs": atlas,
    }, indent=1))
    write_png(grid, gw, gh, out_dir / "label_font_atlas_grid.png", scale=10)

    import os
    if not os.environ.get("MD_KEEP_FRAMES"):
        for _, p in breadcrumb_paths:
            p.unlink()
        for _, _, p in frame_paths:
            p.unlink()
        ppm_dir.rmdir()

    print(f"wrote {out_dir / 'label_font_atlas.json'}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
