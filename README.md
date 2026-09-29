# Machinedrum Module for MPC OS

The Elektron Machinedrum SPS-1 UW sound engine as a native VST2 instrument for Akai MPC OS standalone devices
(built and tested on the Force), with its own touchscreen skin and Q-Link support. All 16 Machinedrum tracks play
from one plugin instance, using the Machinedrum's own DSP code and its own machine, LFO and mixer maths.

<img width="640" height="400" alt="image" src="https://github.com/user-attachments/assets/30d1c99b-333c-4b98-b9e5-2c3339ef0c29" />

**v0.1.0, pre-release.** It plays, saves and reloads with the project, and it is tested on a real Force (MPC OS 3.9.1). There is
no downloadable build: it needs your own Machinedrum firmware, so you build the installer yourself with one script (see
[Building](#building)). Master effects (reverb, delay and the rest of the master section) are not in yet.

Not affiliated with Elektron. Nothing of Elektron's is in this repository or distributed from it; the plugin
needs your own Machinedrum OS 1.63 file and a flash image (see [What you need](#what-you-need)).

## Features

- **16 tracks, one instance.** MIDI notes 36-51 play tracks 1-16 (the Machinedrum's own note-to-track map).
  Each track keeps the Machinedrum's own voice, effects and routing.
- **The machines:** GND, TRX (808-style), EFM, E12, P-I and the ROM sample machines. RAM, INP and MID/CTR machines
  are not offered (no sampling, no audio input and no MIDI output here); a kit that uses one shows a blank bar.
- **Every parameter page of the hardware**, per track:
  - SYN: the eight synth knobs, with the machine's own labels, which change live with the machine.
  - AMP/EFX: AMD, AMF, EQF, EQG, FLTF, FLTW, FLTQ, SRR.
  - ROUTE: DIST, VOL, PAN, DEL, REV and the LFO amounts.
  - LFO: the Machinedrum's per-track LFO, with its destination shown by name.
- **Kits.** The 16 factory kits (extracted from your own flash image at build time), plus any Machinedrum kit
  `.syx` you drop in (see [Kits](#kits)). A new instance starts on the first kit.
- **GLOBAL tab:** a 16-track level mixer, the voice budget, randomise (machines on all, tracks 1-8 or tracks 9-16,
  or a random kit) and the bank and kit selectors.
- **ROM on/off switch** (GLOBAL tab, off by default). Off, tracks on a ROM (sample) machine stay silent and the randomiser leaves ROM
  machines out; each track keeps its ROM setting for when you switch back. ROM machines are the most expensive on the
  Force's CPU, so this is the quickest way to make a busy kit safe.
- **Voice budget, default 4.** The VOICES knob on GLOBAL is a CPU budget, not a plain voice count. Most machines cost
  1 unit and the ROM (sample) machines cost 2, matching what they cost the Force's CPU (measured on the device: a ROM
  voice adds about 400 us per 2.9 ms audio block, the other machines about 210 us). When a trigger would go past the
  budget, the oldest sounding tracks are cut. The default keeps a busy kit inside what one Force core can do; raise it
  if your patterns are sparse, lower it if you hear crackle.
- **Tempo follows MPC's**, so the LFOs stay in time with the project.
- **The skin** is drawn from the Machinedrum's own LCD (fonts, dials, page layout), generated at build time from
  your own firmware, inside a thin hardware-style bezel. Nothing captured from the firmware is stored in the repo.

### Not there yet (known limits)

- **Master effects:** the reverb and delay sends are computed but no effect consumes them, so REV and DEL do nothing.
  The master section (rhythm echo, gate box/reverb, EQ, dynamix) is the next big piece.
- **CPU.** The voices render on two threads (two cores) and the track effects run on the same threads. About 4-5 voices can sound
  at once on a Force with MPC busy: a voice costs roughly 0.3-0.9 ms of a 2.9 ms audio block depending on the machine (ROM, P-I
  and EFM cost the most) and on how busy MPC is. Beyond that the plugin crackles, so the voice budget (default 4) is the guard:
  it cuts the oldest sounding track, tail included, when a new one would go past it. The engine threads run below MPC's own
  audio threads, so overload drops the plugin's own blocks (crackle) rather than MPC's audio or its screen.
- **First load is slower** than later ones (the skin is large: MPC reads and decodes it from the card).
- **ROM machines** are silent unless the sample data was extracted at build time (it is, if you build with your
  flash image). ROM33-48 are empty on the factory image.
- **Latency:** the engine renders 3 blocks (8.7 ms) ahead, which rides out the stalls MPC's own audio threads cause on a busy kit; 2 (5.8 ms) is lower but crackled on the Force.
- Bank and kit are chosen with the arrows for now; a picker list like the machine one is planned for the next
  version.

## What you need

- An MPC OS standalone device (developed on a Force; other MPC OS devices use the same plugin host).
- **Your own Machinedrum OS 1.63 `.syx`** (`Elektron_SPS1-1UW_OS1.63.syx`). This is the sound engine.
- **Your own full flash image** of a Machinedrum UW (8 MB `.bin`), used once at build time for the factory kits and
  the ROM sample memory. Without it you still get every non-ROM machine and any kit `.syx` you add.
- Docker (for the ARM cross-build and the skin renderer), and the sibling checkout of
  [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) for the shared VST wrapper and installer.

## Kits

Put Machinedrum kit sysex files (`.syx`, the MD's own kit dump) in
`/sdcard/vst/machinedrum/kits/` or in `Force Documents/Machinedrum Kits/`. They are picked up within a few seconds.
Each file is a bank; a file with several kits shows them all. The factory kits are the bank called FACTORY.
Master-effect settings inside a kit are ignored for now.

## Building

The build reads your firmware and writes an installer zip containing the plugin, the skin, your extracted kits and ROM
samples, and your OS file (the plugin reads it at run time); the result contains firmware-derived code and data, so it is
for your own devices only.

**Where to run this: on your own computer (macOS or Linux, with Docker and git), not on the Force.** The build runs
inside Docker on your computer, and so does the `git clone` below. The Force is only where the finished plugin is
installed: the `-d <device-ip>` option copies it there over your network and runs the installer, or you copy the zip
over yourself afterwards. Nothing is built or compiled on the device.

```bash
git clone --recursive <this repo> && cd mpc-vst-machinedrum
release/build_release.sh <Elektron_SPS1-1UW_OS1.63.syx> <flash image.bin>            # -> dist/Machinedrum-Module-<version>-mpc-armv7.zip
release/build_release.sh <OS.syx> <flash.bin> -d <device-ip>                          # ...and install it on the Force
```

Options: `-v <version>` (default from `git describe`), `-d <device-ip>` (copy the zip over and run its installer; this stops
and restarts MPC, so save your project first), `-m <mpc-vst-plugins checkout>`. It builds, in order: the x86 helper tools,
the factory kits and ROM samples (by booting the emulated MD from your flash image), the recompiled voice DSP (traced
from your OS file), a **bit-exactness gate** (the recompiled DSP must give the same audio hash as the plain interpreter,
ROM machines included, or nothing is built for the device), the skin, the ARM plugin, and the installer zip in `dist/`.
Prerequisites (each is described at the top of the script): Docker, a checkout of
[mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (fetched automatically if `../mpc-vst` is absent; its `main` has
the dynamic parameter names the plugin needs), the `mdProbe` tool built from `tools/mdtrace` (`MDPROBE`), and Monomodule's
`art.json` for the LCD fonts (`MNM_ART`). To install by hand instead, unzip the result on the device and run `install.sh`
as root; the installer stops MPC, installs, and restarts it, so run it with the device idle.

`HANDOFF.md` has the full state and every step's details (what `mdProbe` is, the recompiler pass, the device
workflow). The skin borrows the Elektron LCD fonts from a
build of Monomodule's skin (its `art.json`, made from a Monomachine OS file), so that is needed for the skin step.

### Plugin catalog

This is a **build-it-yourself** plugin: the installer zip contains firmware-derived code and data, so it is built per user and
must never be published as a release (a catalog entry for it links to this repo and its build instructions, not to a
download). The zip is still catalog-conformant in format: `mpc-plugin.json` (id `machinedrum-module`, license
`AGPL-3.0-only`, source repo) is generated, and the build runs mpc-vst-plugins' `catalog_check.py --catalog` as its last step.
The plugin locates its data next to the `.so` (`MODULE_SUBDIR`), not at a fixed path. Device testing is recorded in
`tested.json` (v0.1.0: Akai Force, MPC OS 3.9.1).

## How it works

The Machinedrum has a ColdFire processor for the sequencer and UI, and two DSP56303 chips: DSP2 renders the 16
voices and DSP1 runs the per-track effects, the mix and the master effects. We keep only the sound path:

- **Voices:** DSP2's program runs unmodified from your OS file in the [dsp56300](https://github.com/sd88me/dsp56300)
  emulator, with a small harness in place of the hardware's DMA and serial loop. All 16 voices render in one block.
- **The ColdFire side** is a C++ host model: the OS tick (smoothing, LFOs, triggers, mixer inputs) with the OS's own
  machine coefficient routines run in a 68k emulator, so the maths is the hardware's.
- **Per-track effects and the mix** (AMD, EQ, filters, SRR, distortion, pan, sends) are a native C++ translation of
  DSP1's code, checked sample by sample against the DSP running the original.
- **Speed on ARM:** the DSP program is statically recompiled ahead of time (from your OS file, so it is never
  shipped) into C++ that the ARM compiler can optimise.

## Development

The work behind this, in order (`HANDOFF.md` has the full log and `docs/` the protocol notes):

1. **Firmware decoded.** The OS `.syx` unpacks into five sections, two of them complete DSP programs.
2. **Protocol traced.** A patched full-system emulation logged every word between the ColdFire and the DSPs. That
   settled the roles (section 1 on DSP2, section 2 on DSP1), the 16 voice slots and the parameter structures.
3. **Voice engine, bit-exact** against the full-system emulation, then the host model (tick, LFOs, triggers).
4. **Mixer chain translated to native C++**, bit-exact against the DSP for 3.2 M samples of the per-track chain and
   384,000 words of the mix.
5. **ARM port of the DSP emulator.** Found and fixed a missing MERGE instruction, a static-initialisation-order bug in
   the opcode tables and a cache-invalidation gap in the boot loader, and proved the ARM output identical to x86 on the
   real Force.
6. **Static recompiler** for the voice DSP: 294% to 141% of real time for a 6-track demo on the Force.
7. **The plugin:** engine thread with real-time scheduling, 524 parameters, SYN labels that follow the machine,
   project save and restore.
8. **The skin, generated from the real LCD** driven in the emulator: font and dial atlases, page layouts, a machine
   picker, the LFO page, chassis and bezel.
9. **Kits and ROM machines:** kit sysex import (factory kits dumped from the emulated MD), and the sample memory the MD
   copies from its flash at boot.
10. **Performance work on the device:** skipping settled silent tracks, ring depth and priority tuning, trimming the skin
    and P memory (about 225 MB less RAM in MPC), the voice budget, skipping idle voices in the DSP loop and silent tracks in the mixer.

## Credits

- **Elektron**, for the Machinedrum. All firmware and sound belong to them; this project ships none of it.
- **shnolk's [Monomodule](https://github.com/shnolk/monomodule)**: the approach this follows (the machine's own
  DSP in an emulator, a host model for the main processor, plugin and skin design), the OS-file decoder adapted in
  `tools/mdfw`, and the Elektron LCD fonts and dial art the skin borrows.
- **[gearmulator-md-mm](https://github.com/joelanders/gearmulator-md-mm)** by Joe Landers: a full-system Machinedrum
  emulation, used as the tracing and correctness reference, and the Musashi 68k core inside it, used by the machine
  routines. Not part of the plugin.
- **[dsp56300](https://github.com/dsp56300/dsp56300)**, the DSP56300 emulator this builds on (our fork,
  [sd88me/dsp56300](https://github.com/sd88me/dsp56300), adds the ARM interpreter and static recompiler work and the
  fixes above), and its bundled asmjit.
- **[mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)**: the shared VST2 wrapper, skin toolchain and
  installer for MPC OS.
- Built with [Claude Code](https://claude.com/claude-code).

Licensed AGPL-3.0-only, like Monomodule (whose decoder is adapted here); dsp56300 is GPL-3.0.
