#!/usr/bin/env bash
# Builds Machinedrum Module's installer zip from YOUR OWN Machinedrum files (nothing of Elektron's is in this repo, and the
# result contains firmware-derived code and data, so it is for your own devices only - never share or publish it).
#
#   release/build_release.sh <OS .syx> <flash .bin> [-v version] [-d device-ip] [-m mpc-vst-plugins checkout]
#
#   -v  version string (default: from `git describe`, e.g. 0.1.0)
#   -d  after building, copy the zip to the Force and run its installer (stops and restarts MPC: save your project first)
#   -m  mpc-vst-plugins checkout (default: $MPC_VST_DIR, ../mpc-vst, else cloned to ~/.cache); its main has the wrapper's
#       "dynamic_name"/"dynamic_display" support this plugin needs, and the catalog checker
# Other inputs: MDPROBE (the mdProbe tool, see tools/mdtrace/README.md; default libs/gearmulator-md-mm/build/...) and MNM_ART
# (mpc-vst-monomodule's vst/build/art.json, the Elektron LCD fonts made from YOUR Monomachine OS; default ../mpc-vst-monomodule/...).
# Needs Docker (images md-armhf-builder and mpc-vst-html-art are built on first use). Output: dist/Machinedrum-Module-<version>-mpc-armv7.zip.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
usage() { sed -n '2,15p' "$0"; exit 2; }
[ $# -ge 2 ] || usage
OS=$(realpath "$1"); FLASH=$(realpath "$2"); shift 2
VERSION=""; DEVICE=""; MV="${MPC_VST_DIR:-}"
while getopts "v:d:m:h" o; do case $o in v) VERSION=$OPTARG;; d) DEVICE=$OPTARG;; m) MV=$OPTARG;; *) usage;; esac; done
if [ -z "$VERSION" ]; then VERSION=$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed -E 's/^v//; s/^([0-9]+\.[0-9]+\.[0-9]+)-.*/\1/'); fi   # commits after a tag: still X.Y.Z (the catalog check needs it); pass -v to name a release
case "$VERSION" in [0-9]*) ;; *) VERSION="0.0.0-dev.$VERSION";; esac
if [ -z "$MV" ]; then
  if [ -d "$ROOT/../mpc-vst" ]; then MV="$ROOT/../mpc-vst"
  else
    MV="$HOME/.cache/mpc-vst-machinedrum/mpc-vst-plugins"
    [ -d "$MV" ] || { mkdir -p "$(dirname "$MV")"; git clone -q https://github.com/sd88me/mpc-vst-plugins.git "$MV"; }
  fi
fi
MV=$(realpath "$MV")
PROBE=$(realpath -m "${MDPROBE:-$ROOT/libs/gearmulator-md-mm/build/source/elektron/md/mdLibTest/mdProbe}")
ART=$(realpath -m "${MNM_ART:-$ROOT/../mpc-vst-monomodule/vst/build/art.json}")
[ -e "$PROBE" ] || { echo "missing: $PROBE (mdProbe: build it first, see tools/mdtrace/README.md 'Build', or set MDPROBE)" >&2; exit 1; }
for f in "$OS" "$FLASH" "$PROBE" "$ART" "$MV/tools/release.py" "$MV/tools/gen_vst.py"; do [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }; done
grep -q "dynamic_name" "$MV/wrapper/vst2_wrap.c" || { echo "$MV's wrapper has no dynamic_name support: use mpc-vst-plugins main (or later)" >&2; exit 1; }
cd "$ROOT"
[ -f libs/dsp56300/source/dsp56kEmu/dsp.h ] || git submodule update --init --recursive
WORK=$ROOT/build-release; mkdir -p "$WORK" vst/build dist
echo "Machinedrum Module $VERSION  (plugins checkout: $MV)"

echo "== 1/7 x86 tools (mdsamples, mdmachine)"
# On the plain interpreter (no JIT): these tools only read memory, and the JIT (x86-64/arm64 hosts, e.g. an Apple Silicon Mac) can
# crash while VoiceEngine initialises where the interpreter does not. Same dir as the tools' output: build-vst-x86/.
cmake -S . -B build-vst-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DDSP56K_NO_JIT_RUNTIME >/dev/null
ninja -C build-vst-x86 mdsamples mdmachine >/dev/null

echo "== 2/7 factory kits and ROM samples (from the flash image, by booting the emulated MD)"
python3 tools/mdkits/make_factory.py "$PROBE" build-vst-x86/mdsamples "$FLASH" "$OS" vst/build/factory

echo "== 3/7 recompiled voice DSP (traced from your OS file and the ROM samples; not shipped as source)"
cmake -S . -B "$WORK/discovery" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMD_DISCOVERY=ON >/dev/null
ninja -C "$WORK/discovery" mdrecomp-discover >/dev/null
mkdir -p "$WORK/recomp"
"$WORK/discovery/mdrecomp-discover" "$OS" "$WORK/recomp/disc.txt" vst/build/factory/ROM_SAMPLES.bin >/dev/null 2>&1
nm -C "$WORK/discovery/mdrecomp-discover" > "$WORK/recomp/nm.txt"
python3 libs/dsp56300/tools/arm32jit_prototype/recomp/recomp_gen2.py "$WORK/recomp/disc.txt" "$WORK/recomp/nm.txt" > "$WORK/recomp/dsp56k_recomp.inl"

echo "== 4/7 bit-exactness gate: the recompiled voice DSP must give the same audio as the plain interpreter"
for v in interp recomp; do
  flags="-DDSP56K_NO_JIT_RUNTIME"; [ $v = recomp ] && flags="$flags -DDSP56K_RECOMP -I$WORK/recomp"
  cmake -S . -B "$WORK/gate-$v" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=$flags" >/dev/null
  ninja -C "$WORK/gate-$v" md-hash >/dev/null
done
H_INTERP=$("$WORK/gate-interp/md-hash" "$OS" vst/build/factory/ROM_SAMPLES.bin 2>/dev/null | grep '^hash')
H_RECOMP=$("$WORK/gate-recomp/md-hash" "$OS" vst/build/factory/ROM_SAMPLES.bin 2>/dev/null | grep '^hash')
echo "   interpreter: $H_INTERP"; echo "   recompiled:  $H_RECOMP"
# Every machine the plugin offers must make sound (TRX XT/CP/MA/CL/XC were silent until their function was given the trigger flag).
SILENT=$(MD_SWEEP=1 "$WORK/gate-recomp/md-hash" "$OS" vst/build/factory/ROM_SAMPLES.bin 2>/dev/null | awk '/^SWEEP/ && $5+0 < 100 && $3 !~ /^(GND--|INP|MID|CTR|RAM)/ && !($3 ~ /^ROM/ && substr($3,4)+0 > 32) {print $3}' | tr '\n' ' ')
[ -z "$SILENT" ] || { echo "GATE FAILED: these offered machines make no sound: $SILENT" >&2; exit 1; }
[ -n "$H_INTERP" ] && [ "$H_INTERP" = "$H_RECOMP" ] || { echo "GATE FAILED: the recompiled build does not match the interpreter - not building for the device" >&2; exit 1; }

echo "== 5/7 skin"
tools/mdskin/build_skin.sh "$OS" "$ART" "$MV" | tail -1

echo "== 6/7 plugin (armhf)"
vst/build_so.sh "$WORK/recomp" "$MV"

echo "== 7/7 installer"
# the plugin reads your OS file from its data dir under this exact name, so the installer carries it (your own file, per-user zip)
rm -rf vst/build/payload && mkdir -p vst/build/payload && cp -r vst/build/factory vst/build/payload/factory && cp "$OS" vst/build/payload/Elektron_SPS1-1UW_OS1.63.syx
python3 "$MV/tools/release.py" --so vst/build/machinedrum_one.so \
  --skin "vst/build/skin/sd88me - VST - Machinedrum Module" --entry vst/build/pluginlist-entry.xml \
  --version "$VERSION" --extra vst/build/payload:vst/machinedrum \
  --id machinedrum-module --repo sd88me/mpc-vst-machinedrum --license AGPL-3.0-only \
  --requires "Your own Machinedrum OS 1.63 file and flash image: this zip is built from them, contains Elektron-derived data and is for your own devices only" \
  --about "Machinedrum Module: the Elektron Machinedrum UW sound engine as an MPC OS instrument (built from your own firmware)" -o dist
ZIP=$(ls dist/Machinedrum-Module-"$VERSION"-*.zip); ls -l "$ZIP"
# catalog conformance (mpc-vst-plugins docs/CATALOG_SPEC.md): the manifest, layout, checksums, ELF/glibc limits
python3 "$MV/tools/catalog_check.py" "$ZIP" --catalog --expect-id machinedrum-module --expect-repo sd88me/mpc-vst-machinedrum

if [ -n "$DEVICE" ]; then
  echo "== installing on $DEVICE (stops and restarts MPC)"
  ssh "root@$DEVICE" 'cat > /tmp/machinedrum-release.zip' < "$ZIP"
  ssh "root@$DEVICE" "cd /tmp && rm -rf Machinedrum-Module-$VERSION* && unzip -o -q machinedrum-release.zip && cd Machinedrum-Module-$VERSION* && sh install.sh -y"
fi
