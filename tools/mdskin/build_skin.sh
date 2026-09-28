#!/usr/bin/env bash
# Builds the skin folder (vst/build/skin/) from the user's own OS files, with tools/mdskin/mk_skin.py (a port of
# mpc-vst-monomodule's own skin generator). Needs Docker (the mpc-vst-html-art image, for Pillow) and an x86 build
# of this repo (build-vst-x86/, for mdmachine).
#   build_skin.sh <MD OS.syx> <monomodule art.json> [mpc-vst checkout] [skin=... ink=... paper=...]
# <monomodule art.json>: mpc-vst-monomodule's vst/build/art.json (its build_skin.sh dumps it from the user's own
# Monomachine OS) - the Elektron LCD fonts and dial the skin is drawn with.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OS=$(realpath "$1"); ART=$(realpath "$2"); MV=$(realpath "${3:-$HOME/mpc-vst}")
mkdir -p "$ROOT/vst/build"
ninja -C "$ROOT/build-vst-x86" mdmachine >/dev/null
"$ROOT/build-vst-x86/mdmachine" "$OS" > "$ROOT/vst/build/machines.txt" 2>/dev/null
cp "$ART" "$ROOT/vst/build/art.json"
rm -rf "$ROOT/vst/build/skin"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e MPC_VST_TOOLS=/mv/tools -v "$ROOT":/r -v "$MV":/mv:ro -w /r mpc-vst-html-art \
  python3 tools/mdskin/mk_skin.py vst/build/art.json vst/build/machines.txt vst/params.json vst/build/skin "${@:4}"
