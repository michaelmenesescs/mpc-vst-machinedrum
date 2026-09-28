#!/usr/bin/env python3
"""Expands a build_dial_atlas.py dial_atlas.json (deduped rotation states, each covering a
value_first..value_last range) into a 128-frame vertical filmstrip PNG, the format
mpc-vst-plugins' shadow_skin.py `knob strip=... frames=128` expects (tools/skin_assets.py
`strip_layout()`: a "stacked down" image, height >= width, one frame per raw parameter value).

This is the last step connecting this session's bit-exact dial-pointer capture (Phase 2 of the skin
plan, HANDOFF.md) to an actual on-device knob widget - previously this data only existed as a
standalone verification artifact (dial_atlas_grid.png), never wired into a real layout.conf build.

usage: build_knob_filmstrip.py <dial_atlas.json> <out.png> [scale]
  scale: integer upscale factor for each frame (default 3 - keeps the bit-exact blocky LCD look
  rather than smoothing it away; MPC will scale the whole strip to whatever the knob's r= implies
  regardless, so this mostly controls source sharpness/file size, not final on-screen size).
"""
import json
import struct
import sys
import zlib
from pathlib import Path


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    atlas = json.loads(Path(sys.argv[1]).read_text())
    out_path = Path(sys.argv[2])
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 3

    steps = atlas["param_steps"]
    cw, ch = atlas["icon_w"], atlas["icon_h"]
    fw, fh = cw * scale, ch * scale

    # value -> state lookup (states cover contiguous ranges and are already sorted by value)
    def state_for(value):
        for st in atlas["states"]:
            if st["value_first"] <= value <= st["value_last"]:
                return st
        return atlas["states"][-1]

    raw = bytearray()
    for value in range(steps):
        rows = state_for(value)["rows"]
        for y in range(fh):
            raw.append(0)  # filter type 0
            sy = y // scale
            row = rows[sy]
            for x in range(fw):
                sx = x // scale
                raw.append(0 if row[sx] == "1" else 255)

    idat = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(">IIBBBBB", fw, fh * steps, 8, 0, 0, 0, 0)
    out_path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
    print(f"wrote {out_path}: {fw}x{fh} per frame x {steps} frames "
          f"({atlas['encoder']}, {len(atlas['states'])} unique states)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
