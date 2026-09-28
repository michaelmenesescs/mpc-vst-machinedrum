#!/usr/bin/env python3
"""Builds the 8 ROUTE page knob filmstrips (bit-exact real-LCD dial pointer icons) the same way
build_amp_fx_knob_strips.py does for AMP/EFX - see that file's docstring and HANDOFF.md's "found the
third per-track page: ROUTE" for how this page was found (the SynthesisEffectsRouting panel button
pressed TWICE, not a different button).

usage: build_route_knob_strips.py <mdProbe binary> <flash.bin> <out dir>
Writes <out dir>/<key>.png for key in dist,del,rev,lfos,lfod,lfom - plus vol/pan reuse the AMP/EFX
build's shared generic look (they're plain knobs elsewhere already, not given their own filmstrip
here, to avoid capturing the same two dials twice).
"""
import subprocess
import sys
from pathlib import Path

SCREEN_ACTION = "SynthesisEffectsRouting+SynthesisEffectsRouting"

# encoder name -> route key, in the ROUTE page's own 4x2 grid order (matches gen_params.py's
# ROUTE_PARAMS - DIST VOL PAN DEL / REV LFOS LFOD LFOM). DataEntryB/C (VOL/PAN) are skipped: those
# params already have knobs elsewhere in the layout, reusing the existing generic look there.
ENCODER_ROUTE_KEY = [
    ("DataEntryA", "dist"), ("DataEntryD", "del"),
    ("DataEntryE", "rev"), ("DataEntryF", "lfos"), ("DataEntryG", "lfod"), ("DataEntryH", "lfom"),
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

    for encoder, key in ENCODER_ROUTE_KEY:
        enc_dir = atlas_dir / encoder
        print(f"-- {key} ({encoder}) --", file=sys.stderr)
        subprocess.run([sys.executable, str(HERE / "build_dial_atlas.py"), mdprobe, flash, str(enc_dir),
                         encoder, SCREEN_ACTION], check=True)
        subprocess.run([sys.executable, str(HERE / "build_knob_filmstrip.py"),
                         str(enc_dir / "dial_atlas.json"), str(out_dir / f"{key}.png")], check=True)

    print(f"{len(ENCODER_ROUTE_KEY)} knob filmstrips written to {out_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
