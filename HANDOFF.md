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
   **Done** (see Status and `docs/PROTOCOL.md`).
   That next pass should also settle the per-voice dispatch pattern and whether per-voice audio is
   separable before DSP2's internal mix — see "Design goal: all voices in one plugin instance" in
   `docs/PROTOCOL.md`. Goal: **Machinedrum One plays all voices at once from one instance** (a MIDI
   note-number drum map), the way Monomodule's Six plays all 6 Monomachine tracks — not one voice per
   plugin instance. Real hardware already renders every voice inside one audio block on the single
   voice-producer DSP, so this should fall out of the per-voice dispatch pattern rather than need
   redesigning later.
2. **Engine.** `md::DspEngine`: load the voice program from the `.syx` (`tools/mdfw`), stub the main loop, and
   drive all voices per audio block (not just one — see step 1's goal). Check sample-for-sample against
   gearmulator-md-mm.
3. **Measure.** Instruction rate for all voices together, on x86 and on the Force. The MD may need
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

- **2026-09-27: runtime protocol decoded (step 1 complete).** Details in `docs/PROTOCOL.md`
  "Runtime protocol". In short:
  - The ColdFire drives both DSPs by **writing fixed-layout structures in their Y memory** (host
    command `P:$12` = DMA block write, `P:$10` = peek), on a 96-sample control tick.
  - **DSP2 = 16 voice slots** at `Y:$800+$40·k`: word 0 = trigger/machine code on the trigger tick,
    words 1-12 = machine coefficients the ColdFire computes from SYN1-8 (+LFO).
  - **DSP2 sends DSP1 16 separate dry mono voice streams** (32-sample blocks per voice). So all
    voices from one instance and per-voice outs are both possible.
  - **DSP1 runs each track's effects page** (AMD, EQ, filter, SRR, distortion; raw params at
    `Y:$200+$40·k`), vol/pan/sends (`Y:$100+5·k`), the mix and the master FX. So Machinedrum One
    needs DSP1's per-track section, not just DSP2.
  - **Load:** DSP2 ~35-60 M instr/s with voices playing (inactive voices ~free); DSP1 ~79 M instr/s
    always. That's roughly 5-7× Monomodule's one track: ~3-4 Force cores if both are emulated.

  **Decisions needed from the user before step 2** (see `docs/PROTOCOL.md` "Design consequences"):
  (1) how to generate the 12 per-machine coefficient words (tables swept from the user's ROM at first
  run, running the ColdFire's own routine in Musashi, or per-machine reverse engineering), and
  (2) the CPU strategy for the Force (native C++ reimplementation of DSP1's per-track chain/mixer vs.
  emulating both DSPs, and whether to target desktop first).

  Tools: `tools/mdtrace` (patches + `mdProbe` scripted driver + analysis scripts; see its README) and
  `tools/mddis` (disassembler for the `.syx` DSP programs).

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
