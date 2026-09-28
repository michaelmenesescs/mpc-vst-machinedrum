#!/usr/bin/env python3
"""Drives mdProbe through named MD screens and saves each as a PNG, for the plugin's skin (the
mk_skin.py-equivalent generator, not yet written - see HANDOFF.md "the skin's foundation"). Never
commit the output: it's Elektron's own LCD art, decoded from the user's own firmware at build time,
same policy as the ROM/flash image and the recompiler's .inl (docs/FIRMWARE.md).

usage: capture_screens.py <mdProbe binary> <flash.bin> <out dir>

Add a screen by adding a (name, [panel actions]) tuple to SCREENS: each name's actions are replayed in
order from a fresh boot (not cumulative - so screens don't depend on capture order or on each other).
"""
import struct
import subprocess
import sys
import zlib
from pathlib import Path

# (screen name, panel: actions to reach it from the home screen)
SCREENS = [
    ("home", []),
    ("syn_page", []),                                    # the default page on boot
    ("amp_fx_page", ["SynthesisEffectsRouting"]),         # AMD EQF EQG FLTF FLTW FLTQ (HostModel params 8-14)
]


def ppm_to_png(ppm_path: Path, png_path: Path) -> None:
    data = ppm_path.read_bytes()
    assert data[:2] == b"P5", "not a binary PGM"

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

    w_tok, i = token(2)
    h_tok, i = token(i)
    _maxv_tok, i = token(i)
    w, h = int(w_tok), int(h_tok)
    i += 1
    pixels = data[i:i + w * h]

    def chunk(tag: bytes, payload: bytes) -> bytes:
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)  # 8-bit grayscale
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0 (none) per scanline
        raw.extend(pixels[y * w:(y + 1) * w])
    idat = zlib.compress(bytes(raw), 9)
    png_path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: capture_screens.py <mdProbe binary> <flash.bin> <out dir>", file=sys.stderr)
        return 2
    mdprobe, flash, out_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    flashcache = out_dir / "_flashcache.bin"  # mdProbe's own cache, reused across screens; not art, still never commit

    for name, actions in SCREENS:
        ppm = out_dir / f"{name}.ppm"
        args = [str(mdprobe), str(flash), str(flashcache)]
        for a in actions:
            args.append(f"panel:{a}")
        args.append(f"lcdpng:{ppm}")
        print(f"capturing {name} ({' '.join(actions) or 'home'})...", file=sys.stderr)
        subprocess.run(args, check=True, capture_output=True)
        ppm_to_png(ppm, out_dir / f"{name}.png")
        ppm.unlink()

    print(f"{len(SCREENS)} screens captured to {out_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
