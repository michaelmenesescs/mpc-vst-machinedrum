#!/usr/bin/env python3
"""Generates the per-machine SYN-label overlay images for the real "Monomodule way" of showing
per-machine knob labels: mpc-vst-monomodule's mk_skin.py bakes one small label-only image per unique
machine, and places all of them as conditionally-visible layout components keyed to the live
`track%d_machine` parameter value via MPC's own `IndexedEnabling` skin feature (confirmed working on
real Force hardware - see /home/sam/mpc-vst/docs/NOTES.md, "Conditional visibility works for VST2
params", and `when=<param>:<option>` in the same doc's "Mode panels" section). This is NOT a dynamic
per-instance render - it's N pre-baked static images, and MPC's skin engine itself picks which one is
visible at runtime by reading the parameter's current value. The dial pointer icons are NOT part of
this overlay - they're value-driven filmstrips (build_dial_atlas.py's output), shared across every
machine, placed as separate components; only the label TEXT differs per machine and needs baking here.

Key finding that makes this tractable for the Machinedrum despite having ~135 machines (vs. the
Monomachine's much shorter machine list): **most machines share an identical 8-label set** (e.g. 52 of
135 machines - all the ROM/sample-player variants - show exactly "PTCH DEC HOLD BRR STRT END RTRG
RTIM"). Deduping by label tuple drops this to ~53 unique overlay images total, not 135 - each track's
`when=` entries can all point at the same small set of shared image files, multiplying only the
layout.conf line count, not the asset count.

usage: gen_syn_label_overlays.py <mdmachine binary> <OS.syx> <label_font_atlas.json> <out dir>
Writes one PNG per unique label-tuple to <out dir>, named by a short hash, plus manifest.json mapping
each machine id -> {"name":, "overlay": "<filename>"}.
"""
import hashlib
import json
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ui_spec import DIAL_GRID  # noqa: E402

CANVAS_W, CANVAS_H = 80, 64  # matches compose_skin.py's dial-grid region (LCD x=48..128)
ORIGIN_X = 48


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
    for y, row in enumerate(rows):
        cy = y0 + y
        if not (0 <= cy < ch):
            continue
        for x, bit in enumerate(row):
            cx = x0 + x
            if 0 <= cx < cw and bit == "1":
                canvas[cy * cw + cx] = 1


def draw_text(canvas, cw, ch, x0, y0, text, font):
    pitch = font["glyph_w"]
    x = x0
    for label_char in text:
        glyph_rows = font["glyphs"].get(label_char.upper())
        if glyph_rows is not None:
            blit(canvas, cw, ch, x, y0, glyph_rows)
        x += pitch


def parse_machines(mdmachine_out: str):
    """mdmachine's list-mode output: '<id> <name> fn=$xxxxxx P1 P2 .. P8' (params right-padded)."""
    machines = {}
    for line in mdmachine_out.splitlines():
        m = re.match(r"\s*(\d+)\s+(\S+)\s+fn=\$[0-9a-f]+\s+(.*)$", line)
        if not m:
            continue
        mid, name, rest = m.groups()
        params = (rest.split() + [""] * 8)[:8]
        machines[int(mid)] = (name, params)
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
    print(f"{len(machines)} machines parsed", file=sys.stderr)

    grid = DIAL_GRID
    images_by_key = {}   # label-tuple key -> filename
    manifest = {}
    for mid, (name, params) in machines.items():
        key = tuple(params)
        if key not in images_by_key:
            canvas = bytearray(CANVAS_W * CANVAS_H)
            for i, label in enumerate(params):
                if not label:
                    continue
                col, row = i % grid["cols"], i // grid["cols"]
                lx = grid["dial_x0"] + col * grid["label_col_pitch"] - ORIGIN_X
                ly = grid["label_y0"] + row * grid["dial_row_pitch"]
                draw_text(canvas, CANVAS_W, CANVAS_H, lx, ly, label, font)
            digest = hashlib.sha1("|".join(params).encode()).hexdigest()[:10]
            fn = f"syn_labels_{digest}.png"
            write_png(canvas, CANVAS_W, CANVAS_H, out_dir / fn, scale=4)
            images_by_key[key] = fn
        manifest[mid] = {"name": name, "params": params, "overlay": images_by_key[key]}

    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(f"{len(images_by_key)} unique overlay images for {len(machines)} machines "
          f"-> {out_dir / 'manifest.json'}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
