#!/usr/bin/env python3
"""Builds a dial-pointer icon atlas (JSON, build output only - never committed, see docs/FIRMWARE.md)
for Phase 2 of the "Monomodule way" skin plan (HANDOFF.md, "Skin plan: the Monomodule way"). Same
technique as `build_font_atlas.py`'s font extraction, applied to a knob icon instead of a character:
sweep a parameter's full range via its front-panel rotary encoder, capture the LCD after every step,
and dedupe consecutive identical frames (within a fixed crop region) into the bounded set of distinct
rotation states the dial icon actually draws - no ROM icon-table decoding needed.

Confirmed empirically (2026-09-28): turning `PanelEncoder::DataEntryA` on the AMP/EFX page
(`panel:SynthesisEffectsRouting`) moves the AMD knob's dial pointer *and* shows a temporary numeric
readout under it - useful as an independent ground truth when eyeballing results, though this script
doesn't depend on reading that overlay itself (see build_font_atlas.py's HANDOFF.md postmortem on
trusting only self-consistent relabeling - here we sidestep that risk entirely by keying icon states
by first/last swept *value*, which comes from the sweep loop itself, not from re-reading the screen).

usage: build_dial_atlas.py <mdProbe binary> <flash.bin> <out dir> [encoder] [screen_action]
  encoder      PanelEncoder name (default DataEntryA - the AMD knob on the AMP/EFX page)
  screen_action  panel: action(s) to reach the screen first, "+"-joined for repeats of the same
                 button (default SynthesisEffectsRouting; the ROUTE page is
                 "SynthesisEffectsRouting+SynthesisEffectsRouting" - see HANDOFF.md, "found the third
                 per-track page")
"""
import json
import subprocess
import struct
import sys
import zlib
from pathlib import Path

# The AMP/EFX page's 8 dials (AMD AMF EQF EQG / FLTF FLTW FLTQ SRR, mapped to encoders
# DataEntryA..H) sit in a uniform 4-col x 2-row grid - confirmed this session by variance-scanning
# three of the eight (DataEntryA/B/E) and finding the same 14x12 icon box just offset by a constant
# pitch: x0 = 52 + col*21, y0 = 14 + row*31. Re-derive with the variance-scan technique (see
# HANDOFF.md) rather than trusting this for a different screen or a non-4x2 layout.
_ENCODER_GRID_INDEX = {name: i for i, name in enumerate(
    ["DataEntryA", "DataEntryB", "DataEntryC", "DataEntryD",
     "DataEntryE", "DataEntryF", "DataEntryG", "DataEntryH"])}


def dial_crop_for(encoder: str):
    i = _ENCODER_GRID_INDEX[encoder]
    col, row = i % 4, i // 4
    x0 = 52 + col * 21
    y0 = 14 + row * 31
    return (x0, x0 + 14), (y0, y0 + 12)


PARAM_STEPS = 128  # AMD (and most MD params) is a 0-127 range


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
        print("usage: build_dial_atlas.py <mdProbe binary> <flash.bin> <out dir> [encoder] [screen_action]",
              file=sys.stderr)
        return 2
    mdprobe, flash, out_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    encoder = sys.argv[4] if len(sys.argv) > 4 else "DataEntryA"
    screen_action = sys.argv[5] if len(sys.argv) > 5 else "SynthesisEffectsRouting"
    out_dir.mkdir(parents=True, exist_ok=True)
    flashcache = out_dir / "_flashcache.bin"

    ppm_dir = out_dir / "_dial_frames"
    ppm_dir.mkdir(exist_ok=True)

    # screen_action may be several panel: presses joined with "+" (e.g. the ROUTE page needs the same
    # SynthesisEffectsRouting button pressed twice - see ui_spec.py's SCREENS["route"]["reach"]).
    args = [str(mdprobe), str(flash), str(flashcache)]
    for action in screen_action.split("+"):
        args.append(f"panel:{action}")
    frame_paths = [ppm_dir / "f0.ppm"]
    args.append(f"lcdpng:{frame_paths[0]}")
    for step in range(1, PARAM_STEPS):
        p = ppm_dir / f"f{step}.ppm"
        args += [f"encoder:{encoder}:1", f"lcdpng:{p}"]
        frame_paths.append(p)

    print(f"driving mdProbe through {PARAM_STEPS} steps of {encoder}...", file=sys.stderr)
    subprocess.run(args, check=True, capture_output=True)

    (x0, x1), (y0, y1) = dial_crop_for(encoder)
    cw, ch = x1 - x0, y1 - y0

    states = []  # list of {"value_first", "value_last", "rows": [...]}
    last_key = None
    w = h = None
    for value, p in enumerate(frame_paths):
        fw, fh, pix = load_ppm(p)
        w, h = fw, fh
        rows = ["".join("1" if pix[y * w + x] < 128 else "0" for x in range(x0, x1)) for y in range(y0, y1)]
        key = tuple(rows)
        if key == last_key:
            states[-1]["value_last"] = value
        else:
            states.append({"value_first": value, "value_last": value, "rows": rows})
            last_key = key

    print(f"{len(states)} distinct icon states across {PARAM_STEPS} values", file=sys.stderr)

    grid_cols = 10
    grid_gap = 2
    grid_rows = (len(states) + grid_cols - 1) // grid_cols
    gw = grid_cols * cw + (grid_cols + 1) * grid_gap
    gh = grid_rows * ch + (grid_rows + 1) * grid_gap
    grid = bytearray([255]) * (gw * gh)
    for i, st in enumerate(states):
        r, c = divmod(i, grid_cols)
        gx0 = grid_gap + c * (cw + grid_gap)
        gy0 = grid_gap + r * (ch + grid_gap)
        for y in range(ch):
            for x in range(cw):
                grid[(gy0 + y) * gw + (gx0 + x)] = 0 if st["rows"][y][x] == "1" else 255

    (out_dir / "dial_atlas.json").write_text(json.dumps({
        "encoder": encoder, "screen_action": screen_action,
        "icon_w": cw, "icon_h": ch,
        "crop_x": [x0, x1], "crop_y": [y0, y1],
        "param_steps": PARAM_STEPS,
        "states": states,
    }, indent=1))
    write_png(bytes(grid), gw, gh, out_dir / "dial_atlas_grid.png")

    for p in frame_paths:
        p.unlink()
    ppm_dir.rmdir()

    print(f"wrote {out_dir / 'dial_atlas.json'}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
