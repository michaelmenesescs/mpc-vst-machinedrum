#!/usr/bin/env bash
# Builds machinedrum_one.so for the Force (armhf), self-contained (just this repo's submodules + the
# CMakeLists.txt at its root), inside the md-armhf-builder image (built automatically from
# tools/Dockerfile.armhf-builder on first use).
#   build_so.sh <dir holding dsp56k_recomp.inl> <mpc-vst-plugins checkout> [armhf|aarch64]
# armhf (default): Gen1 (Force, MPC Live II, X, One...), outputs vst/build/<so>. aarch64: Gen2 (MPC Live III...), outputs
# vst/build/aarch64/<so> via the md-aarch64-builder image; there the DSP runs on the plain interpreter plus the recompiled blocks
# (-DDSP56K_NO_JIT_RUNTIME: the configuration the bit-exactness gate checks), not the dsp56300 JIT.
# The recompiled DSP embeds firmware words: it is generated from the user's own OS file
# (libs/dsp56300/tools/arm32jit_prototype/recomp/) and never shipped -- REC below must point at your own
# build of it (see HANDOFF.md, "recompiler discovery pipeline" - tools/mdrecomp/mdrecomp_discover.cpp +
# recomp_gen2.py).
set -euo pipefail
rp() { (cd "$1" && pwd); }   # no realpath on older macOS
REC=$(rp "$1"); MV=$(rp "$2"); ARCH=${3:-armhf}
case $ARCH in
  armhf)   IMG=md-armhf-builder;   DOCKERFILE=Dockerfile.armhf-builder;   TC=armhf.cmake;   STRIP=arm-linux-gnueabihf-strip; OUT=build; JITFLAG="";;
  aarch64) IMG=md-aarch64-builder; DOCKERFILE=Dockerfile.aarch64-builder; TC=aarch64.cmake; STRIP=aarch64-linux-gnu-strip;   OUT=build/aarch64; JITFLAG=" -DDSP56K_NO_JIT_RUNTIME";;
  *) echo "usage: build_so.sh <recomp dir> <mpc-vst-plugins> [armhf|aarch64]" >&2; exit 2;;
esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)   # repo root (this script lives in vst/)
if [ ! -f "$ROOT/libs/dsp56300/source/dsp56kEmu/dsp.h" ] || [ ! -f "$ROOT/libs/gearmulator-md-mm/source/mc68k/CMakeLists.txt" ]; then
  echo "submodules missing -- run: git -C '$ROOT' submodule update --init --recursive" >&2
  exit 1
fi
docker image inspect $IMG >/dev/null 2>&1 || docker build -q -t $IMG -f "$ROOT/tools/$DOCKERFILE" "$ROOT" >/dev/null
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/vst.json" --params-h
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/tap/vst.json" --params-h
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/tapfx/vst.json" --params-h
mkdir -p "$ROOT/vst/$OUT"
OBJ=$([ $ARCH = armhf ] && echo obj || echo obj-$ARCH)
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$REC":/rec:ro -v "$MV":/mv:ro $IMG sh -c "
  cmake -S /r -B /r/vst/build/$OBJ -G Ninja -DCMAKE_TOOLCHAIN_FILE=/r/tools/$TC -DCMAKE_BUILD_TYPE=Release \
    -DMPC_VST_DIR=/mv '-DCMAKE_CXX_FLAGS=-DDSP56K_RECOMP$JITFLAG -I/rec' >/dev/null &&
  ninja -C /r/vst/build/$OBJ machinedrum_one machinedrum_tap machinedrum_tapfx md-vst-smoke md-hash 2>&1 | tail -5 &&
  for f in machinedrum_one machinedrum_tap machinedrum_tapfx; do $STRIP -o /r/vst/$OUT/\$f.so /r/vst/build/$OBJ/\$f.so; done"
for f in machinedrum_one machinedrum_tap machinedrum_tapfx; do OUTF="$ROOT/vst/$OUT/$f.so"; if command -v md5sum >/dev/null 2>&1; then md5sum "$OUTF"; else md5 -r "$OUTF"; fi; done
