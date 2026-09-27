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

- **2026-09-27: decisions made, and the coefficient routines found.** The user chose:
  (1) **the MD's own routines** for the voice coefficients, and (2) **a bit-exact native C++
  translation** of DSP1's per-track effect chain and mixer (fixed-point arithmetic matching the
  DSP56300, verified sample by sample against the emulated DSP1), rather than emulating DSP1.

  (1) turned out to be cheap and clean: the OS has a 135-entry machine descriptor table whose
  coefficient functions are pure `fn(out, params)` C functions (see `docs/PROTOCOL.md`, "Host
  model: the ColdFire side"). Plan: load section 0 of the user's `.syx` into a Musashi instance and
  call the function for the voice's machine. Remaining for (1): translate how the OS builds the
  per-track 24-value parameter array (`a6`: kit value scaling, LFO, smoothing, pitch) and the tick
  routine's DSP1 values.

  **Next steps:**
  1. Find where `a6` is built (watch writes to the per-track parameter arrays; start from the tick
     routine around `$20b1a0` and its callers) and translate the LFO/smoothing code.
  2. Prototype `md::VoiceEngine`: DSP2 alone in dsp56300 (program from section 1), a harness that
     writes the 16 voice slots directly into Y memory and reads the ESSI0 link (16 × 32-sample
     blocks), Musashi calling the machine functions. Check it sample-for-sample against md-mm.
  3. Disassemble DSP1's per-track chain (hot loops at `$16e-$196`, `$95c-$9a0`) and start the
     bit-exact C++ translation, with a DSP1-in-emulator reference test.
  4. Measure DSP2 alone on the Force (static recompiler).

- **2026-09-27: step 1 of the host model done: the per-track parameter pipeline is decoded.**
  Smoothing (same slew as the Monomachine), per-track LFO (tempo-relative oscillator, 8 shape
  functions, mix and depth) and the LFO apply that builds each voice's 24-value parameter array
  are all self-contained routines in the OS; they run per sequencer tick (64 per beat at 120 BPM).
  Measured cost of running them plus the machine functions in a 68k emulator: ~1 M 68k
  instructions/s in total. Plan (see `docs/PROTOCOL.md`, "Host model plan: hybrid"): our C++ owns the
  tick schedule and inputs; the MD's own routines do the maths in Musashi. Next: translate the tick
  routine's per-voice orchestration (triggers, accent/velocity, DSP1 per-track values), then
  prototype `md::VoiceEngine` (step 2 below).

- **2026-09-27: voice engine prototype works, bit-exact.** `engine/VoiceEngine` (DSP2 alone from the
  `.syx`, Monomodule-style harness, 16 voice outputs per 32-sample block) matches md-mm's output
  sample for sample; `engine/MachineRunner` (the OS's machine functions in Musashi) reproduces the
  slot words exactly. Build with `tools/build_proto.sh` (x86, against md-mm's built libraries);
  tools `mdvoice` and `mdmachine`. Details in `docs/PROTOCOL.md`, "Voice engine prototype".

  **Next:**
  1. Host model (`engine/HostModel`): tick schedule (64 per beat), smoothing + LFO via the OS's own
     routines in `MachineRunner`'s CPU (load the internal-SRAM routine copy from OS `$2622f4` to
     `$1000000`), trigger codes, then per voice `compute()` → `VoiceEngine::setSlot()`. Translate the
     tick routine's trigger/accent/velocity handling and DSP1 per-track values (`$20af52-$20b44c`).
  2. End-to-end test: kit + triggers through HostModel + VoiceEngine vs. md-mm (mdProbe ESSI dump).
  3. Measure every machine family's DSP2 cost (EFM, E12, P-I, ROM); ROM/RAM machines also need the
     user's sample data (flash, not in the `.syx`).
  4. DSP1 per-track chain: disassemble and start the bit-exact C++ translation.
  5. Port VoiceEngine to `libs/dsp56300` (arm32 branch): needs md-mm's DSP fixes (MERGE, DMA) checked.

- **2026-09-27: host model, first version, bit-exact end to end.** `engine/HostModel` (tick schedule,
  machine assignment applied at trigger, trigger codes, per-voice machine functions, smoothing and
  LFO via the OS's own routines) + `engine/VoiceEngine`: TRX-B2 and TRX-SD each 6,400/6,400 samples
  identical to md-mm. Tick scheduling decoded: DSP2-driven interrupts, CPU-bound ~120 Hz, not tempo
  synced (see `docs/PROTOCOL.md`, "Host model: tick scheduling"). Tool: `mdhost`.

  **Next:**
  1. ~~LFO configuration and trigger restart~~ done 2026-09-27: `HostModel::setLfo()`, trigger
     flag + the tick's trigger path; tick-by-tick identical to md-mm with an LFO on PTCH.
  2. ~~Measure DSP2 cost per machine family~~ done: +1.5-3.6 M instr/s per playing voice over a 6.2 M/s
     baseline (docs/PROTOCOL.md "DSP2 cost per machine"). Next for cost: skip silent voices in the
     harness.
  3. Mixer DSP: translate the `Y:$100+5·k` computation (volume/velocity/accent, pan, sends); then
     the bit-exact C++ translation of DSP1's per-track chain.
  4. ROM/RAM machines: sample data from the user's flash (not in the `.syx`).
  5. Port to `libs/dsp56300` (arm32) and measure on the Force.

- **2026-09-27: silent voices skipped in the harness.** `VoiceEngine::installHarness` now redirects
  the per-voice render call to a fast clear when the voice's persisted machine code is 0 (never
  triggered) or 1 (the empty machine GND--, which all tracks default to at boot) — both cases
  already output 32 zeros, so this only removes the cost of getting there. Confirmed
  byte-identical output to the pre-patch engine for 200 blocks, both idle and with a playing voice.
  Baseline (16 silent voices) 6.2 → 2.7 M instr/s; one playing voice + 15 idle 8.0 → 4.8 M instr/s.
  Details and a gotcha (this assembler's `beq`/`bra` take a raw relative displacement, not an
  address) in `docs/PROTOCOL.md`.

  **Next:**
  1. ~~Mixer DSP: translate the `Y:$100+5·k` computation~~ done; ~~bit-exact C++ translation of DSP1's
     per-track chain~~ done (see the next entry).
  2. ROM/RAM machines: sample data from the user's flash (not in the `.syx`).
  3. Port `VoiceEngine` to `libs/dsp56300` (arm32) and measure on the Force.

- **2026-09-27: mixer DSP per-track chain translated, bit-exact; host model sends the mixer words.**
  - `HostModel` computes each track's DSP1 inputs as the OS tick does: the 9 effect words and the 5
    mix words (route; VOL gain from level, velocity/accent and VOL; PAN; REV; DEL), with the OS's own
    level slew (`$100029e`) and new `trigger(track, velocity, accent)`, `setLevel`, `setMute`,
    `setRouting`. Identical to md-mm's DSP1 writes.
  - `engine/TrackFx` (+ `engine/Dsp56.h`, DSP56300 fixed-point helpers): AMD, EQ, both filter
    sections, SRR, distortion, translated from DSP1's per-track function. **3.2 M samples and all
    state identical** to the DSP's own code in the emulator (`tools/mdmix`: `MixerRef` reference,
    `mdfxtest`), fixed and moving parameters. 16 tracks ≈ 3% of one x86 core.
  - Details in `docs/PROTOCOL.md`, "Mixer DSP inputs" and "Mixer DSP per-track chain".

  **Next:**
  1. ~~The mix~~ done; ~~wire it together~~ done (next entry).
  2. End-to-end comparison with md-mm's audio output (needs the master FX, below).
  3. ROM/RAM machines: sample data from the user's flash (not in the `.syx`).
  4. Port `VoiceEngine` to `libs/dsp56300` (arm32) and measure on the Force.

- **2026-09-27: the mix translated, bit-exact; the engine renders audio.**
  - `engine/Mixer`: pan law, VOL gain, reverb/delay sends, individual-output routing, from DSP1's
    mix code (`$294-$341`, `$9de` and the code it generates). 384,000 words identical to the DSP's
    own code (`mdmixtest`).
  - `engine/Engine`: HostModel → VoiceEngine → 16 × TrackFx → Mixer; outputs dry main L/R, the two
    sends, the individual outputs and each track's post-effects signal. `tools/mdrender` renders a
    demo pattern to a WAV: 8 s in ~1.3 s on x86, nearly all of it the voice DSP emulation.

  **Next:**
  1. Machinedrum FX: translate the master section (`P:$344-$970`) the same way; then One + FX can be
     compared end to end with md-mm's audio.
  2. Port `VoiceEngine` to `libs/dsp56300` (arm32) and measure on the Force: the voice DSP is now the
     only emulated part and the whole cost.
  3. ROM/RAM machines: sample data from the user's flash (not in the `.syx`).
  4. The plugin itself (wrapper, skin, `vst.json`) following `mpc-vst-monomodule`.

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
