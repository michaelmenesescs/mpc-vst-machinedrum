# mdtrace

A local investigation tool: it captures every HI08 word the emulated Machinedrum ColdFire sends
to and reads from each DSP, by patching a few log points into gearmulator-md-mm's `mddsp.cpp`.
Not part of gearmulator-md-mm; not upstreamed; nothing here touches the submodule's git history.

It answered the question this port most needed answered: **which OS-file section (see
`docs/FIRMWARE.md`) runs on which DSP.** See `docs/PROTOCOL.md` for the result.

## Use

Requires a full Machinedrum flash image (`elektron_sps1-1uw_os1.63.bin` or equivalent), not the
`.syx` (mdTraceTool loads a raw ROM image via `md::RomLoader`, the same way md-mm's own firmware
tests do). Never commit the ROM or a trace captured from it: a trace contains the DSP program
verbatim.

```bash
cd libs/gearmulator-md-mm
git apply ../../tools/mdtrace/mddsp_trace.patch
git apply ../../tools/mdtrace/mdLibTest_CMakeLists.patch
cp ../../tools/mdtrace/mdTraceTool.cpp source/elektron/md/mdLibTest/
git submodule update --init --depth 1 \
    source/dsp56300 source/mc68k source/cpp-terminal \
    source/dsp56300/source/asmjit source/3rdparty/freetype source/3rdparty/RmlUi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -Dgearmulator_BUILD_JUCEPLUGIN=OFF -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF -DBUILD_TESTING=ON
cmake --build build --target mdTraceTool
MD_TRACE=1 ./build/source/elektron/md/mdLibTest/mdTraceTool /path/to/elektron_sps1-1uw_os1.63.bin [note] [frames]
```

`mdTraceTool` boots the device, runs `frames` audio frames of "boot+idle", then (unless `note` is
0) a note-on and a note-off, each for `frames` frames. `MD_TRACE=1` turns on the per-word log to
stderr; without it the tool just checks the device boots. 44100 frames ≈ 1 emulated second is
enough to capture a whole DSP program upload; the boot alone needs `note 0` and a few hundred
thousand frames to also reach steady-state runtime traffic (see `docs/PROTOCOL.md`).

The patch adds five log points, each gated by `MD_TRACE`, to `md::Dsp` in `mddsp.cpp`:
`BOOT dspN word=...` (the tiny first-stage loader upload via the standard dsp56300 `DspBoot`
Length/Address/Data protocol), `BOOT dspN DONE ...`, `RUN dspN UC->DSP word=... cycle=...`
(ColdFire to DSP, post-boot), `RUN dspN DSP->UC word=... cycle=...` (DSP to ColdFire), and
`RUN dspN IRQ vba=... cycle=...` (host-command interrupts). It also adds an unconditional exit-time
counter summary, useful for sanity-checking a run without wading through the log.
