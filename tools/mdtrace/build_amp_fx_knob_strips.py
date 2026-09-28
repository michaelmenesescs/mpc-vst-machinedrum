#!/usr/bin/env python3
"""Builds the 8 AMP/EFX page knob filmstrips (bit-exact real-LCD dial pointer icons, Phase 2 of the
skin plan, HANDOFF.md) as vst/gen_layout.py-ready assets: one 128-frame strip per FX param, named by
its fx key so gen_layout.py can reference `strip=build/knobs/<fxkey>.png frames=128` directly.

Wraps build_dial_atlas.py (capture + dedupe) and build_knob_filmstrip.py (dedupe -> 128-frame strip)
so this is the single entry point vst/build_so.sh (or a developer) needs to run before a layout build.
Never commits its output - like every other tools/mdtrace asset, it's derived from the user's own
firmware capture (docs/FIRMWARE.md) and build*/ is already gitignored.

usage: build_amp_fx_knob_strips.py <mdProbe binary> <flash.bin> <out dir>
Writes <out dir>/<fxkey>.png for fxkey in amd,amf,eqf,eqg,fltf,fltw,fltq,srr.
"""
import subprocess
import sys
from pathlib import Path

# encoder name -> fx key, in the AMP/EFX page's own 4x2 grid order (matches vst/gen_layout.py's
# FX_PARAMS and gen_params.py's FX_PARAMS - AMD AMF EQF EQG / FLTF FLTW FLTQ SRR)
ENCODER_FX_KEY = [
    ("DataEntryA", "amd"), ("DataEntryB", "amf"), ("DataEntryC", "eqf"), ("DataEntryD", "eqg"),
    ("DataEntryE", "fltf"), ("DataEntryF", "fltw"), ("DataEntryG", "fltq"), ("DataEntryH", "srr"),
]

HERE = Path(__file__).parent


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    mdprobe, flash, out_dir = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    atlas_dir = out_dir / "_dial_atlases"
    atlas_dir.mkdir(exist_ok=True)

    for encoder, fxkey in ENCODER_FX_KEY:
        enc_dir = atlas_dir / encoder
        print(f"-- {fxkey} ({encoder}) --", file=sys.stderr)
        subprocess.run([sys.executable, str(HERE / "build_dial_atlas.py"), mdprobe, flash, str(enc_dir), encoder],
                       check=True)
        subprocess.run([sys.executable, str(HERE / "build_knob_filmstrip.py"),
                         str(enc_dir / "dial_atlas.json"), str(out_dir / f"{fxkey}.png")], check=True)

    print(f"8 knob filmstrips written to {out_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
