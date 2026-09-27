# mdtrace

Local investigation tooling: it instruments gearmulator-md-mm to log what the emulated Machinedrum
ColdFire sends each DSP, and drives the emulated machine from a script. Not part of gearmulator-md-mm;
not upstreamed; nothing here is committed to the submodules. Results are in `docs/PROTOCOL.md`.

Never commit the ROM, the cached flash image, or any trace/dump captured from them: they contain the
DSP program and firmware-computed data verbatim.

## Files

- `gearmulator-md-mm.patch`: apply in `libs/gearmulator-md-mm`. Adds to `mddsp.cpp` the HI08 log
  points (`BOOT`/`RUN` lines, gated by `MD_TRACE=1` or `md::g_mdTraceOn`) and a DMA log (`DMA dspN
  Y:addr=value`, every word a host wrote into DSP memory via DMA); to `mdhardware.cpp` a dump of the
  DSP2→DSP1 ESSI0 link (`MD_ESSI_DUMP=path`, gated by `md::g_mdEssiDumpOn`); to `mdhardware.h` a
  `traceDsp(i)` accessor; and the two tools below to `mdLibTest/CMakeLists.txt`.
- `dsp56300-md-mm.patch`: apply in `libs/gearmulator-md-mm/source/dsp56300`. Adds the DMA write hook
  (`dsp56k::g_hostDmaWriteHook`) that the DMA log uses.
- `mdProbe.cpp`: the main tool. Boots `md::Hardware` (first-run flash initialisation is done once and
  cached to a file you name), advances 20 emulated seconds (`MDPROBE_BOOT=frames` to change), then
  runs a script. Copy to `source/elektron/md/mdLibTest/`.
- `mdTraceTool.cpp`: the earlier, plugin-level tool used for the boot/DSP-role trace.
- `analysis/*.py`: parsers for the logs (`state.py` per-step changed Y addresses, `voice.py` DSP2
  voice-slot history, `sweep.py` parameter-sweep diff, `dma.py` block shapes).

## Build

```bash
cd libs/gearmulator-md-mm
git apply ../../tools/mdtrace/gearmulator-md-mm.patch
cp ../../tools/mdtrace/mdProbe.cpp ../../tools/mdtrace/mdTraceTool.cpp source/elektron/md/mdLibTest/
git submodule update --init --depth 1 source/dsp56300 source/mc68k source/cpp-terminal \
    source/3rdparty/freetype source/3rdparty/RmlUi
(cd source/dsp56300 && git submodule update --init --depth 1 source/asmjit \
    && git apply ../../../../tools/mdtrace/dsp56300-md-mm.patch)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -Dgearmulator_BUILD_JUCEPLUGIN=OFF -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF -DBUILD_TESTING=ON
cmake --build build --target mdProbe
```

Important: a raw ROM image boots into first-run flash initialisation, and the machine only plays
after about 20 emulated seconds of `Hardware::advance()`. Driving it any shorter, or through
`synthLib::Plugin` without `setHostSamplerate`/`setBlockSize`, gives silence and no voice traffic.

## mdProbe script

`mdProbe ROM.bin FLASHCACHE action...`, each action its own argument:

| Action | Does |
|---|---|
| `trace:on` / `trace:off` | HI08 + DMA log to stderr |
| `essi:on` / `essi:off` | DSP2→DSP1 link dump (needs `MD_ESSI_DUMP=path`) |
| `wait:N` | process N frames (prints the audio peak) |
| `adv:N` | `advance()` N frames (no audio) |
| `trig:T[:HOLD]` | press front-panel trigger T (1-16) for HOLD frames |
| `note:CH:NOTE:VEL`, `off:CH:NOTE`, `cc:CH:CC:VAL` | MIDI (CH 0-based) |
| `sysex:HEX` | e.g. `sysex:f000203c02005b001000f7` (assign machine: track, machine, table) |
| `panel:NAME` | tap Kit, Enter, Exit, Up, Down, Left, Right, Play, Stop, Function, Record |
| `lcd` | print the LCD as ASCII |
| `prof:N` | N frames with a PC histogram and instruction counts for both DSPs |

Track parameters by CC on channel 0 (track 1): 16-23 SYN1-8, 24-32 AMD AMF EQF EQG FLTF FLTW FLTQ SRR
DIST, 33-36 VOL PAN DEL REV, 37-39 LFOS LFOD LFOM, 8 level.

## mddis

`tools/mddis/mddis.cpp` disassembles one DSP program straight from the `.syx` (`mddis OS.syx 1|2
[from to]...`, hex P addresses). Its output is firmware: don't commit it. Link it against the md-mm
build's `dsp56kEmu`:

```bash
MM=libs/gearmulator-md-mm; B=$MM/build/source
g++ -std=c++17 -O1 -I$MM/source/dsp56300/source -I$MM/source/dsp56300/source/dsp56kEmu -I$MM/source \
  tools/mddis/mddis.cpp tools/mdfw/Firmware.cpp $B/dsp56300/source/dsp56kEmu/libdsp56kEmu.a \
  $B/dsp56300/source/dsp56kBase/libdsp56kBase.a $B/baseLib/libbaseLib.a \
  $B/dsp56300/source/asmjit/libasmjit.a $B/dsp56300/source/vtuneSdk/libvtuneSdk.a -lpthread -ldl -o mddis
```
