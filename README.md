# mpc-vst-machinedrum

The Elektron Machinedrum SPS-1 UW sound engine as an Akai Force/MPC OS VST, built the way
[Monomodule](https://github.com/shnolk/monomodule) builds the Monomachine: the MD's own DSP code runs in the
[dsp56300](https://github.com/sd88me/dsp56300) emulator, and a small C++ host model stands in for the main
processor. The sequencer, UI and ColdFire OS are not emulated.

Planned products:

- **Machinedrum One**: the voice machines (sound modules only), from the voice DSP program.
- **Machinedrum FX**: the master effects as a separate effect plugin, from the mixer/FX DSP program.

**Status: research.** The OS file decodes and holds two complete DSP programs; the CPU-to-DSP protocol is not
mapped yet. See `HANDOFF.md` for where this stands and `docs/FIRMWARE.md` for the OS file format.

## Layout

- `tools/mdfw`: decodes a Machinedrum OS `.syx` (sysex, flash container, aPLib sections, DSP records). Adapted
  from Monomodule's decoder.
- `libs/dsp56300`: `sd88me/dsp56300`, branch `arm32` (the static recompiler work from `mpc-vst-monomodule`).
- `libs/gearmulator-md-mm`: [joelanders/gearmulator-md-mm](https://github.com/joelanders/gearmulator-md-mm),
  unmodified. A full-system MD emulation (ColdFire + both DSPs), used as the reference for tracing the
  protocol and checking our output. Not part of the plugin.

## Firmware

Nothing from Elektron is in this repository. You need your own Machinedrum OS 1.63 file
(`Elektron_SPS1-1UW_OS1.63.syx`); the tracing step also uses a full flash image. Never commit either, or
anything generated from them (decoded sections, recompiled `.inl`).

```bash
g++ -std=c++17 -O2 tools/mdfw/mdfw.cpp tools/mdfw/Firmware.cpp -o mdfw
./mdfw Elektron_SPS1-1UW_OS1.63.syx --records
```

Not affiliated with Elektron. Licensed AGPL-3.0, like Monomodule (whose decoder is adapted here); dsp56300 is
GPL-3.0.
