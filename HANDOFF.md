# Handoff

Read this first in any new session. Keep it current at every checkpoint.

## The plan

Follow Monomodule's design (see `mpc-vst-monomodule`): run only the MD's DSP code in dsp56300, with a harness
stub in place of the hardware DMA/ESSI main loop, and a C++ host model in place of the ColdFire. Split into two
plugins: the voice machines (One) and the master effects (FX).

The difference from Monomodule: we don't have to reverse-engineer the host protocol from disassembly alone.
gearmulator-md-mm already boots the whole MD, so we can log the real ColdFire-to-DSP traffic.

Steps:

1. **Trace (x86).** Build gearmulator-md-mm (console or test target, no plugin needed) with a full flash
   image. Log which OS program is booted into which DSP, then every HI08 word and host command to each DSP
   while triggering one track with known machines and parameters. Output: the MD's parameter protocol (its
   equivalent of Monomodule's 52-word block) and the voice-DSP/FX-DSP roles. **This step decides feasibility.**
2. **Engine.** `md::DspEngine`: load the voice program from the `.syx` (`tools/mdfw`), stub the main loop, and
   drive one voice. Check sample-for-sample against gearmulator-md-mm.
3. **Measure.** Instruction rate per voice and for all 16 voices, on x86 and on the Force. The MD may need
   voice subsets or the static recompiler (`libs/dsp56300`, `arm32` branch) to fit a core.
4. **FX engine** from the mixer program, the same way.
5. **Port**: VST wrapper, skin and `vst.json`, following `mpc-vst-monomodule` and `mpc-vst-plugins`.

## Status

- **2026-09-27: OS file decoded.** `tools/mdfw` unpacks the MD OS 1.63 `.syx` into five sections, two of them
  complete DSP programs. Details and open questions in `docs/FIRMWARE.md`.

- **2026-09-27: step 1 (trace) done for the DSP-role question; runtime protocol still open.** Built
  gearmulator-md-mm's mdLib/test targets on x86 (plugin build off, `BUILD_TESTING=ON`; needs the
  `dsp56300`, `mc68k`, `cpp-terminal`, `asmjit`, `freetype`, `RmlUi` submodules initialized — see
  `tools/mdtrace/README.md`). It boots and passes `mdAudioFirmwareTest` against the user's full MD
  1.63 flash image.

  Added `tools/mdtrace`: a local patch to `mddsp.cpp` (not committed to the submodule, not
  upstreamed) that logs every UC↔DSP HI08 word, plus a driver tool (`mdTraceTool`). Capturing 5
  emulated seconds of boot and comparing the real transfer to `tools/mdfw`'s decoded sections
  **confirmed which OS-file section runs on which DSP**: section 1 (large) → DSP2 (voice producer);
  section 2 (small) → DSP1 (mixer/codec/master FX). This also confirms `tools/mdfw` decodes
  byte-exactly, and settles the split: **Machinedrum One** (voices) comes from section 1 on DSP2,
  **Machinedrum FX** from section 2 on DSP1, exactly the two-plugin split the user wants (master FX
  dropped from One, shipped as its own plugin). Also found: the boot has two stages, a tiny
  first-stage loader over the standard `dsp56300` boot protocol, then the real program streamed as
  ordinary runtime host-port words read by DSP-side loader code. Full details in
  `docs/PROTOCOL.md`.

  **Not yet done: the runtime (post-boot) parameter protocol** — the MD equivalent of Monomodule's
  52-word block. Both DSPs reach real runtime traffic within the 5-second capture (350K+ runtime
  words, 36K host-command IRQs on top of the program transfer), but what any of it means is
  undecoded. That's the next tracing session, and it's the hard part: correlate known
  machine/parameter changes (driven via sysex or the front panel, see md-mm's `mdautomation.cpp`/
  `mdsysexautomation.cpp` for how to script that) against the word stream, the way Monomodule's
  `HostModel.cpp` was worked out for the Monomachine.

## Relationship between the projects

Monomodule (Shnolk) and gearmulator-md-mm (Joe Landers) share no code and neither credits the other. md-mm is
full-system emulation (Musashi ColdFire + two DSP56303s, 8 MB flash image); Monomodule runs one DSP from the
public OS file with a hand-written host model. Their dsp56300 fixes don't overlap: Monomodule added SR.SM
saturation, MPYRI and PFLUSH; md-mm added MERGE and DMA/ESSI fixes. The MD engine may need both sets; check when
the voice program first runs.

## Firmware handling

The user owns the hardware and has supplied the OS `.syx` and a full flash `.bin` (MD OS 1.63). They're kept
outside the repo. `.gitignore` blocks `*.syx`, `*.bin` and `*.inl`; never commit firmware or anything derived
from it.
