#!/bin/sh
# Build the prototype tools (x86, against gearmulator-md-mm's already-built libraries, which carry the MD
# emulator fixes). First build md-mm as described in tools/mdtrace/README.md (target mdProbe or mdLib).
# Outputs to ${OUT:-build-proto}/: mdfw, mdvoice, mdmachine, mdhost.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
MM=$R/libs/gearmulator-md-mm
B=$MM/build/source
OUT=${OUT:-$R/build-proto}
mkdir -p "$OUT"
DSP_LIBS="$B/dsp56300/source/dsp56kEmu/libdsp56kEmu.a $B/dsp56300/source/dsp56kBase/libdsp56kBase.a $B/baseLib/libbaseLib.a $B/dsp56300/source/asmjit/libasmjit.a $B/dsp56300/source/vtuneSdk/libvtuneSdk.a"
DSP_INC="-I$MM/source/dsp56300/source -I$MM/source/dsp56300/source/dsp56kEmu -I$MM/source -I$MM/source/dsp56300/source/asmjit/src -DASMJIT_STATIC"
CXX=${CXX:-g++}
$CXX -std=c++17 -O2 "$R/tools/mdfw/mdfw.cpp" "$R/tools/mdfw/Firmware.cpp" -o "$OUT/mdfw"
$CXX -std=c++17 -O2 $DSP_INC "$R/tools/mdvoice/mdvoice.cpp" "$R/engine/VoiceEngine.cpp" "$R/tools/mdfw/Firmware.cpp" $DSP_LIBS -lpthread -ldl -o "$OUT/mdvoice"
$CXX -std=c++17 -O2 -I"$MM/source" -I"$MM/source/mc68k" "$R/tools/mdmachine/mdmachine.cpp" "$R/engine/MachineRunner.cpp" "$R/tools/mdfw/Firmware.cpp" "$B/mc68k/lib68kEmu.a" "$B/baseLib/libbaseLib.a" -lpthread -o "$OUT/mdmachine"
$CXX -std=c++17 -O2 $DSP_INC -I"$MM/source/mc68k" "$R/tools/mdhost/mdhost.cpp" "$R/engine/HostModel.cpp" "$R/engine/MachineRunner.cpp" "$R/engine/VoiceEngine.cpp" "$R/tools/mdfw/Firmware.cpp" "$B/mc68k/lib68kEmu.a" $DSP_LIBS -lpthread -ldl -o "$OUT/mdhost"
echo "built: $OUT/mdfw $OUT/mdvoice $OUT/mdmachine $OUT/mdhost"
$CXX -std=c++17 -O2 $DSP_INC "$R/tools/mdmix/mdmix.cpp" "$R/tools/mdmix/MixerRef.cpp" "$R/tools/mdfw/Firmware.cpp" $DSP_LIBS -lpthread -ldl -o "$OUT/mdmix"
echo "built: $OUT/mdmix"
$CXX -std=c++17 -O2 $DSP_INC "$R/tools/mdmix/mdfxtest.cpp" "$R/tools/mdmix/MixerRef.cpp" "$R/engine/TrackFx.cpp" "$R/tools/mdfw/Firmware.cpp" $DSP_LIBS -lpthread -ldl -o "$OUT/mdfxtest"
echo "built: $OUT/mdfxtest"
