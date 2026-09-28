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
  2. ~~Port `VoiceEngine` to `libs/dsp56300` (arm32)~~ done (next entry); still needed: measure on the
     Force (no physical device in this session).
  3. ROM/RAM machines: sample data from the user's flash (not in the `.syx`).
  4. The plugin itself (wrapper, skin, `vst.json`) following `mpc-vst-monomodule`.

- **2026-09-27: engine cross-compiles and runs correctly for 32-bit ARM (the Force).** No physical
  device in this session (sandboxed container) — verified with cross-compilation + `qemu-arm`, not
  on-device timing. `docs/PROTOCOL.md`, "Force port" has the details; summary:
  - One real fix needed: on a target with no JIT, this fork's interpreter opcode cache must be
    turned on explicitly (`setInterpreterEnabled(true)`) or `exec()` calls through a null instruction
    pointer on the first instruction. Done in `VoiceEngine`'s constructor (guarded by
    `!dsp56k::g_useJIT`, so x86 is unaffected).
  - **Portability proved**: this fork's interpreter, forced on x86 too
    (`-DDSP56K_NO_JIT_RUNTIME`), matches the armhf cross-build byte-for-byte on the full engine
    (8 s demo render). The port itself is sound.
  - **Found and fixed a real gap**: `op_Merge` was an unimplemented stub in `libs/dsp56300`; ported
    the real implementation from gearmulator-md-mm's separate fork, adapted to this fork's
    accumulator representation. Pushed to `sd88me/dsp56300` branch `armhf-interp-merge-fix`
    (this repo's `libs/dsp56300` submodule now points there). Not exercised by the current demo kit.
  - **Open, not blocking**: bisecting the demo kit found that TRX-SD (only, of 6 machines checked)
    produces different DSP2 audio between this fork and gearmulator-md-mm's fork, diverging after
    2 blocks. Not yet root-caused (Tcc and LRA inspected and ruled out). Since this session never
    checked TRX-SD's *audio* against real hardware (only its coefficient words, elsewhere), it's
    not yet known which fork is right. Needs either a real hardware capture of TRX-SD or an
    instruction-level trace diff between the two forks to resolve.
  - **Still needed**: real Force timing (this session has no device access) — re-run the static
    recompiler's discovery step (`libs/dsp56300`'s `tools/arm32jit_prototype/recomp/`, ~3.8-3.9x
    over the interpreter for Monomachine machines) against DSP2's program, once on-device.

- **2026-09-27: real Force timing, first number, and cross-arch bit-exactness confirmed on
  hardware.** `mdrender_arm` copied to the Force and run against the user's OS `.syx`: **8.0 s of the
  6-track demo kit rendered in 23.6 s (294% of real time)**, interpreter only, no static recompiler —
  and it produced the exact same WAV as an x86 build of the same commit forced onto the plain
  interpreter (`-DDSP56K_NO_JIT_RUNTIME`, same trick as the earlier qemu check): both `md5
  bcaf9ded0bc6ea4659a6eacb939f0cf1`. So the ARM port's determinism claim (previously only checked
  under qemu) now holds on the real device too.

  294% is ~3x too slow for 6 of 16 tracks, but expected at the interpreter-only stage: HANDOFF's own
  plan was always interpreter-for-correctness-first, static-recompiler-for-speed-second. If DSP2/DSP1
  get a similar speedup to the ~3.8-3.9x the recompiler measured for Monomachine machines, that's
  ~77% of real time — inside budget. Confirming this is now the load-bearing next step, not a
  nice-to-have.

  One build wrinkle worth keeping: `tools/build_proto.sh`'s x86 reference build links against
  md-mm's *own* dsp56300 fork, which predates `setInterpreterEnabled`/the ARM port's other API
  additions — building the current `engine/` sources against it fails to compile. The x86 side of
  this check instead built `libs/dsp56300` (our arm32 fork, the same one `mdrender_arm` used) natively
  for x86, with the JIT forced off. `build_proto.sh` itself still targets md-mm's fork and hasn't been
  updated; do that (or note the split) before relying on it again for anything touching `VoiceEngine`.

  **Next:** static recompiler discovery pass (`libs/dsp56300/tools/arm32jit_prototype/recomp/`)
  against DSP2's program from the user's `.syx`, then measure the recompiled version on the Force the
  same way.

- **2026-09-27: recompiler discovery pipeline adapted to Machinedrum; generated program crashes at
  DSP init, not yet root-caused.**
  - New `tools/mdrecomp/mdrecomp_discover.cpp`: a discovery tracer for DSP2 (`VoiceEngine` alone,
    same generic `DSP::s_recompTraceHook`/`getRecompInfo` infrastructure `arm32jit_prototype/recomp`
    already provides — it isn't Monomodule-specific). Drives every machine in the OS's descriptor
    table (skipping id 0/1, never rendered) through 6 coefficient sweeps each, with a mid-decay
    retrigger, to exercise parameter-dependent branches. `recomp_gen2.py` on the trace: **1527
    blocks, 98.4% instruction coverage, 264 whole-loop blocks**, from a 6.7 s x86 run.
  - Built a gated x86 gate build (`-DDSP56K_RECOMP -I<dir with dsp56k_recomp.inl>
    -DDSP56K_NO_JIT_RUNTIME`, our own `libs/dsp56300` fork — see the build-split note two entries
    up). **It segfaults before rendering anything**, inside `VoiceEngine`'s constructor/`reset()`
    (program init, not even the demo pattern): `op_ResolveCache` dereferences a null
    `OpcodeInfo*` for a bogus opcode word (`0x000800`) at PC `$65`. The generated `.inl` has no
    `recompBlock<$65>` (the block before it, at `$64`, is one word and should fall through to `$65`
    normally) — so `$65` must be running the plain interpreter, reading a P-memory word that isn't
    what's really there at that point in execution. Not yet resolved; candidates not yet checked:
    whether the opcode-cache entry struct's layout changes size under `DSP56K_RECOMP` in a way one
    translation unit doesn't agree with (ODR/ABI mismatch), or whether `VoiceEngine::installHarness`'s
    P-memory patching interacts with the recompiled-block dispatch's word-verification differently
    than plain interpretation.
  - **Not a dead end**: the discovery pipeline itself (tracer, generator, coverage) worked correctly
    and is reusable; the bug is in what runs after, specific to enabling `DSP56K_RECOMP` for this
    program. Needs isolating (bisect which of the 1527 blocks is actually active near PC $64-$70;
    or try recomp with a trimmed `.inl` containing only later, more-exercised blocks, to check
    whether the whole mechanism or just this early one is broken) before it's safe to cross-compile
    and try on the Force.
  - Housekeeping: this session downloaded `gdb` + its runtime deps as loose `.deb`s (via
    `apt-get download` + `dpkg-deb -x`, no root) into `/tmp/gdbroot`, since apt/dpkg needs root and
    wasn't available interactively. Not installed system-wide, nothing added to the repo.

  **Next:** root-cause the segfault (see candidates above) before re-measuring on the Force.

- **2026-09-27: recompiler segfault root-caused and fixed (two real bugs, both in shared
  `libs/dsp56300`, not Machinedrum-specific) — recompiled DSP2 verified bit-exact and measured on
  the Force: 294% → 141% of real time.**
  - **Bug 1**: `Opcodes::getFieldInfo()`'s backing table (`g_runtimeFieldInfos`, `opcodes.cpp`) was a
    namespace-scope global. Any translation unit that also constructs its own `Opcodes` object at
    global/static-init scope (our discovery tracer does, mirroring `mnm_recomp_discover.cpp`) races
    it: C++ doesn't order dynamic initialization between TUs, so which one runs first is an
    accident of link order. When ours ran first, every opcode field lookup silently failed and the
    tracer misclassified real instructions as invalid. Root-caused by writing a ~10-line minimal
    repro (`Opcodes g_ops;` at namespace scope, one lookup) that reproduced with the library alone,
    no engine code involved — confirming it wasn't Machinedrum-specific. **Fixed**: made
    `g_runtimeFieldInfos` a function-local static in `getFieldInfo()` (construct-on-first-use,
    immune to cross-TU ordering).
  - **Bug 2, the actual crash**: the DSP56300 boot-protocol program loader (`dspBootCode.cpp`, used
    to stream DSP2's program in over HI08 — see "Runtime protocol" above) writes P memory directly
    and invalidates only the JIT's block-chain cache (`Jit::notifyProgramMemWrite`), never the
    static recompiler's per-block verification cache (`DSP::notifyProgramMemWrite` →
    `recompInvalidate`) — because it bypasses `memWriteP`, the one path that calls both. A block
    that verified true against not-yet-fully-streamed P words then stayed cached as "verified"
    forever, since nothing ever invalidated it once the rest of the boot transfer overwrote those
    same words with the real program — so the recompiled dispatch ran stale, wrong code, corrupting
    execution until it landed on a bogus opcode and crashed. **Fixed**: the boot loader's per-word
    write now also calls `DSP::notifyProgramMemWrite` (made public for this; was private, only
    reached internally via `memWriteP`). Likely latent in `libs/dsp56300` generally, not just for
    Machinedrum — anything whose program load goes through this exact boot-protocol path and then
    gets recompiled could hit it; Monomodule's tooling apparently loads differently (or never
    revisited the affected addresses before boot fully finished) and never tripped it.
  - Both fixes are in `libs/dsp56300`, pushed to `sd88me/dsp56300` branch `recomp-fixes` (built on
    top of the earlier MERGE-op fix commit, same as that branch).
  - **Verified**: rebuilt discovery (`tools/mdrecomp/mdrecomp_discover.cpp`, still 8228 distinct
    instructions, now with correct lengths — 840 blocks instead of 1527, 97.3% coverage, since
    correct lengths merge more instructions per block), rebuilt `mdrender` x86 with `-DDSP56K_RECOMP`:
    **byte-identical WAV** (md5 `bcaf9ded...`) to the plain-interpreter x86 build. Cross-compiled for
    armhf and ran on the real Force: same md5 there too, and **8.0 s rendered in 11.3 s — 141% of
    real time**, down from the interpreter-only 294% measured two entries up. Roughly a 2.1x
    speedup on-device (less than the ~3.8-3.9x Monomachine measured; DSP2's program/instruction mix
    differs, and only 97.3% of instructions got recompiled here).
  - Still 41% over budget for this 6-of-16-track demo kit; a full 16-track pattern needs more. Not
    yet tried: whether coverage or block quality improves with a broader discovery sweep (more
    machines' edge cases, or ROM/RAM machines once flash sample data is available), or whether the
    remaining 2.7% uncovered instructions are concentrated in something hot.

  **Next:**
  1. ~~Push both `libs/dsp56300` fixes to a branch~~ done (`recomp-fixes`). Consider upstreaming bug 2
     (boot-protocol invalidation gap) since it's a real, generally-applicable correctness bug, not
     Machinedrum-specific.
  2. Try to close the gap to real-time: wider discovery coverage, and/or measure where the
     remaining time actually goes (per-machine cost, same as the interpreter-only breakdown earlier
     in this doc) now that the recompiled build is trustworthy to profile.
  3. DSP1 (mixer/FX) is already native C++, not part of this — this was all DSP2 (voice engine).
  4. ROM/RAM machines: still need the user's flash sample data.
  5. The plugin itself (wrapper, skin, `vst.json`).

- **2026-09-28: full 16-track kit measured on the Force — 231% of real time, the gap is bigger than
  the 6-track number suggested.** Built an ad-hoc benchmark (not committed — a throwaway variant of
  `tools/mdrender/mdrender.cpp` with all 16 tracks assigned instead of 6: a mix of TRX, EFM and E12
  machines, several with dense 16th-note patterns, no silent tracks). Recompiled build, same
  `.inl` as the 141%-measurement above: **29% of real time on x86, 231% on the real Force** for 8 s
  of this pattern. So a genuinely busy full kit is over 2x too slow even with the static
  recompiler — the earlier 141% (6 of 16 tracks, some idle) undersold the gap for a real
  performance target.
  - Not yet done: a per-machine-family cost breakdown on the recompiled build (the interpreter-only
    breakdown in "DSP2 cost per machine" above is stale now that the recompiler changes the constant
    factor). That's the next useful measurement before deciding where to spend further optimization
    effort — e.g. whether cost concentrates in a few expensive machine families (letting some voices
    stay interpreted while cheap ones get the recompiler's full benefit is not how it works today:
    recompilation is program-wide, not per-machine) or is roughly uniform.
  - Given the scope of closing this gap further (profiling, and likely real work on interpreter
    per-instruction overhead or the discovery sweep's coverage), this is a substantial next
    investigation in its own right, not a quick follow-up — left for the next session rather than
    rushed here.

- **2026-09-28: per-machine cost profiled on the recompiled build (x86) — no single hot machine;
  costs are fairly uniform, so the fix isn't "avoid machine X."** Single voice active, others
  silent (already near-free per the harness), 2000 blocks/machine, several coefficient sweeps:
  **baseline (all 16 silent) 24.3 µs/block; cheapest active machine ~24.6-24.7 µs (+0.3, matches
  "inactive/settled voices are nearly free"); most expensive (P-IRC, P-IMT, P-IHH, P-ISD, EFMHH,
  P-ICC, TRXMA — all ~40-42 µs) only ~1.7x the baseline.** A block's real-time budget is 32/44100 s
  = 725.6 µs; even 16 simultaneous worst-case machines wouldn't come close to that on this x86 box
  (~307 µs), yet the real 16-track pattern measured 231% (~1676 µs/block-equivalent) **on the
  Force** — consistent with the Force's interpreter being roughly 6.5-8x slower than this x86 dev
  machine for the same workload (matches the ratio between every x86/Force pair measured so far:
  45%→294% and 19%→141% for the 6-track kit, 29%→231% for the 16-track kit). That's an ARM-vs-x86
  raw-speed gap, not a code hotspot — no amount of "which machines are cheaper" analysis closes it
  by itself.
  - **Concrete next lever, not yet attempted**: split the 16 voice slots across the Force's multiple
    CPU cores. DSP2 is one emulated chip processing all 16 slots per call, but nothing prevents
    running *two or more `VoiceEngine` instances* in parallel, each fed only a subset of the 16
    slots (the rest left silent, which the existing silent-voice harness optimization already makes
    nearly free in that instance) — each thread's cost ≈ baseline + its own subset's active-voice
    cost, on separate cores. This was flagged as a possibility from the very start of this project
    (see "The plan", step 3: "The MD may need voice subsets... to fit a core") and the profiling
    here confirms per-voice costs are additive and roughly independent, which is exactly what a
    split like this needs to be worth doing. Splitting 16 into 2-4 groups could plausibly close most
    or all of the 231%→100% gap. Not implemented: this is a real architecture change (multiple
    engine instances, per-block thread sync, merging each instance's 8 real + 8 silent-slot outputs
    into one), worth its own planning session rather than rushing unsupervised.

- **2026-09-28: multi-core voice split prototyped (`engine/ParallelVoiceEngine`); real hardware
  test possibly caused the Force to reboot — treat this as unconfirmed but concerning, not as a
  working result.**
  - Implemented `ParallelVoiceEngine` (splits the 16 voice slots round-robin across N
    `VoiceEngine` instances, one `std::thread` per group per block, merged by voice index — silent
    voices in a group are free, same as the existing single-engine harness optimization) and
    templated `HostModel`/`Engine` on the voice-engine type (`HostModel<TVoices>`, `EngineT<TVoices>`,
    with `Engine = EngineT<VoiceEngine>` and `ParallelEngine = EngineT<ParallelVoiceEngine>` aliases)
    so both the existing single-engine path and this one share all the code above VoiceEngine.
    Verified on x86: output is bit-identical regardless of group count (1/2/4), confirming the
    split/merge logic is correct; not a speed win on x86 (that dev box is fast enough that
    per-block thread spawn overhead exceeds the compute saved).
  - **The Force has 2 cores, not the 3-4 this project's early docs guessed** (`nproc` = 2) — so at
    most a 2-way split is possible here, not 4-way.
  - Cross-compiled and copied to the device to measure `groups=1` (238% — consistent with the
    231% measured earlier, same workload) and `groups=2`. **The device rebooted during or right
    after the `groups=2` run** (`uptime` showed ~1 minute afterward; no persistent journal survived
    the reboot to show a cause). MPC came back up and was running normally afterward, and this
    session's files were cleaned off `/tmp`, but **the cause is not confirmed** — could be this
    test (spawning extra threads once per 32-sample/725µs block, ~11,000 times over the 8 s test,
    on a `PREEMPT_RT` kernel that may be running the audio path at real-time priority — thread
    creation at that rate on that kernel is a real suspect) or an unrelated device event. **Do not
    re-run `groups=2` (or any multi-threaded variant) against the physical Force without the user
    present and aware of this**, and don't treat the x86 groups=1/2/4 numbers above as informative
    for the real device — they were never meaningfully compared on-device before the reboot.
  - Before trying this on hardware again: per-block `std::thread` creation is the wrong design
    regardless of the reboot question — at 725µs/block, spawn+join overhead alone is likely a
    meaningful fraction of budget on this hardware. A real implementation needs a persistent
    thread pool (threads created once, woken per block via a condition variable or similar), not
    threads spawned fresh every block. `ParallelVoiceEngine::renderBlock` as written now is a
    correctness prototype only, not close to production-ready.

- **2026-09-28: adjustable voice cap added (`HostModel::setMaxActiveVoices`), as a lower-risk
  alternative to the multi-core split above — real 16-simultaneous-voice patterns are unlikely in
  practice, so bounding worst-case load this way avoids the threading/reboot question entirely.**
  - `HostModel<TVoices>::setMaxActiveVoices(int)` (1-`kTracks`; default `kTracks` = disabled,
    identical to current behavior — verified bit-exact, same md5 as every prior render). Tracks the
    N most-recently-triggered tracks; a new distinct trigger past the cap steals the
    least-recently-triggered one by force-triggering it to the OS's own empty machine (id 0,
    `GND--`, no params) — using the normal `MachineRunner::compute()` + `VoiceEngine::setSlot()`
    path, not a raw memory hack, so it stays correct by construction. The victim's assigned machine
    (`m_machine`/`m_pendingMachine`) is untouched, so its next real trigger plays normally; this is
    a hard cut (no fade), which is the standard tradeoff for simple voice-stealing, not something
    fixed here.
  - **Measured and it's more nuanced than hoped**: on the stress-test 16-track pattern (all 16
    tracks retriggering almost every 16th note — much busier than realistic use), lowering the cap
    from 16 down to 2 plateaus around 19-20% of real time on x86 (was 29% uncapped) and doesn't go
    lower, because capping only skips DSP2's *render* cost for stolen voices — it doesn't skip
    `MachineRunner::compute()` (the 68k-emulated machine coefficient function), which still runs on
    every trigger regardless of whether that voice ends up immediately silenced. For this
    artificially dense pattern, that computation cost turns out to be comparable to DSP2's own
    render cost.
  - **Reframing the actual target**: this stress pattern (every track retriggering continuously) is
    not how a real Machinedrum kit gets programmed. The original sparse 6-track demo pattern -
    much more representative - is already comfortably inside budget on the recompiled build (45%→19%
    on x86; by the established ~6.5-8x x86/Force ratio, likely 50-60% on the Force, i.e. real-time
    with headroom, not yet directly re-measured on-device since the last hardware session ended out
    of caution after the reboot). So for realistic use, the voice cap is insurance against pathological
    edge cases, not a required fix.
  - **Next, if pushing the stress-test number further matters**: profile `MachineRunner::compute()`'s
    cost per call (already known aggregate, ~1 M 68k instructions/s total, from "Host model: first
    version" above, but not broken out per-trigger under heavy simultaneous-retrigger load) and
    consider skipping it for voices already marked for stealing before computing, not just after.

- **2026-09-28: step 5 (the plugin) started — Machinedrum One's `mpc_engine()`, offline-verified.**
  Following `mpc-vst-monomodule`'s architecture: `vst/engine.cpp` (all 16 voices from one instance,
  MIDI note → track, a persistent render thread — not per-block spawn, unlike yesterday's
  `ParallelVoiceEngine` prototype — feeding a ring buffer the host's audio callback drains). V1
  params are deliberately minimal (per track: machine id, level, pan; global: tempo, max_voices);
  per-track FX/LFOs aren't exposed yet. Top-level `CMakeLists.txt` + `cmake/dsp56300.cmake` mirror
  `mpc-vst-monomodule`'s structure (an `MPC_VST_DIR`-gated plugin target, plus `vst/smoke.cpp` for
  an offline end-to-end test — see `mpc-vst-plugin` skill's pipeline, step 4).
  - **First end-to-end test caught two real bugs** neither of which any earlier tool (mdrender,
    mdrendercap, mdrenderpar) had exercised, since those always set every per-track param
    explicitly: "level" was wired to `HostModel::setLevel` (a separate kit LEV knob), not param 17
    VOL, which is what the mixer's own gain formula actually reads — silent regardless of
    everything else. And FLTW (filter width) defaults to 0 (closed) unless set, same as EQF/EQG —
    now defaulted per track in `engine.cpp` (matching `mdrender.cpp`'s working demo kit), since
    they aren't yet exposed as VST params. Fixed; verified real audio (peak 5710/32767), zero
    underruns, ~12% CPU for 8 s of real time on x86.
  - **Deliberately stopped here for this session**: no `.so` build yet (needs `MPC_VST_DIR` +
    the armhf Docker toolchain from `mpc-vst-plugins`), no skin/`TUI.json`, nothing touching the
    physical Force. Given yesterday's reboot incident, any step from here that reaches the device
    (bench, deploy, register/restart) should happen with the user present, per the `mpc-vst-plugin`
    skill's own "ask before restarting MPC" rule.
  - **Next:** build the actual `.so` (offline, no device) via `mpc-vst-plugins`' `tools/build_port.sh`
    or this repo's own CMake + `MPC_VST_DIR`; run the skill's `tools/test_port.sh` (ASan/UBSan host
    test) and `tools/bench.sh` before any device step; expose per-track FX/LFO params; then a skin.

- **2026-09-28: `machinedrum_one.so` built for the Force, verified offline under QEMU.**
  `vst/build_so.sh` (+ `tools/Dockerfile.armhf-builder`, `tools/armhf.cmake`) mirrors
  `mpc-vst-monomodule`'s own armhf cross-build: `debian:bookworm`'s `crossbuild-essential-armhf`
  (glibc ~2.36, staying below the Force's 2.39) rather than this session's own local
  `arm-linux-gnueabihf` toolchain, which is too new for a *shared* library that must dynamically
  link against the device's already-loaded glibc (fine for the static standalone test binaries
  built with it earlier, wrong for this).
  - Found and fixed a real bug this surfaced: `libs/gearmulator-md-mm/source/mc68k` was a PUBLIC
    include dir on `mdcore`, leaking into `vst2_wrap.c`'s plain-C compile and shadowing the
    sysroot's real `<endian.h>` with mc68k's own (C++-only) one. Now PRIVATE.
  - Verified: one exported symbol (`VSTPluginMain`), highest `GLIBC_2.36` (device has 2.39),
    libstdc++ statically linked (matches the skill's `-fvisibility=hidden`/`-Bsymbolic`/
    `--exclude-libs` setup for multiple plugin instances sharing MPC's process). Ran the `.so` +
    `md-vst-smoke` under Docker's QEMU-backed `--platform linux/arm/v7` against the user's own
    `.syx`: real audio (peak 14578/32767), no crash — a functional check only, not a timing
    measurement (QEMU emulation speed says nothing about the real device).
  - **Still nothing touching the physical Force**: no deploy, no `MPC.settings` edit, no restart.
    Per the skill's own rules and the reboot incident two entries up, that step needs the user
    present.
  - **Next:** the skill's `tools/test_port.sh` (ASan/UBSan) if it can be adapted to this repo's
    CMake-based build rather than its own generic `sources` compile step; per-track FX/LFO params;
    a skin; then, with the user present, `tools/bench.sh` and the actual device deploy/register.

- **2026-09-28: the skin's foundation — bit-exact real LCD capture, confirmed working, following the
  same idea as Monomodule's skin but a more direct route for this project.** Monomodule's
  `mnm_artdump.cpp` gets pixel-exact LCD art by calling upstream Monomodule's own reverse-engineered
  font/icon decoders (`RomArt.cpp`/`SpecData.cpp`) on the user's OS file — years of prior community
  work this project doesn't have an equivalent of for the Machinedrum. But `gearmulator-md-mm`
  *full-system emulates the real LCD controller*, so the real ColdFire OS's own UI code, running in
  Musashi exactly as on hardware, produces real pixels we can just read
  (`md::FrontPanel::getLcdPixel(x,y)`, 128x64, 1-bit) — no font reverse-engineering needed at all.
  - Confirmed end to end: built `mdProbe` (`tools/mdtrace`, needs only `mdLib` — no RmlUi/freetype/
    cpp-terminal despite the full project needing those submodules present to configure) against
    the user's full flash image, and added a new `lcdpng:PATH.ppm` action (alongside the existing
    ASCII `lcd` action) that writes the real framebuffer as a binary PGM. Converted to PNG (a few
    lines of `zlib`, no ImageMagick/PIL needed) and viewed: **a pixel-perfect capture of the real
    Machinedrum's home/track screen** — BPM readout, LEV/PTCH/DEC/RAMP/HOLD column labels, kit
    number, track name, pattern name, exactly as real hardware renders it.
  - **Never commit captured LCD art** (this session's test screen was viewed then deleted, never
    added to the repo) — same policy as the ROM/flash image and the recompiler's `.inl`: it's
    Elektron's own content, per-user build data only. A real skin-generation script (this project's
    analog of Monomodule's `mk_skin.py`) would call `mdProbe` with `panel:`/`sysex:` actions to
    reach each screen it needs (track select, parameter pages, kit browser, ...) and `lcdpng:` to
    capture each one, at build time, from the user's own firmware — never bundling the images
    themselves.
  - **Not yet done** (the actual `mk_skin.py`-equivalent is real, scoped work, not attempted this
    session): enumerating which MD screens the plugin's `TUI.json` needs, the `panel:`/`sysex:`
    sequences to reach each one, and the Python generator that composites captured LCD crops (plus
    the skin's own knob/button chrome, likely via the `mpc-vst-plugin` skill's
    `layout.conf`/`shadow_skin.py` path) into `TUI.json` + PNGs.
  - Housekeeping: this needed the user's full 8 MB flash `.bin` (not just the `.syx`), already on
    disk from the earlier tracing session (`/home/sam/roms/machinedrum/`), and a flash-cache file
    (`mdProbe`'s second argument) that `mdProbe` generates on first run and reuses after — also
    derived-from-firmware, also never committed.

- **2026-09-28: real screen navigation confirmed; a reusable capture tool committed.**
  `mdProbe`'s `panel:` action only recognized 11 hand-picked button names; rebuilt its map from the
  full `PanelControl` enum via `panelControlName()`, so every real MD button (`Track1`-`6`,
  `BankGroup`/`A`-`D`, `Tempo`, `SynthesisEffectsRouting`, `DataPageForward`/`Backward`, `Scale`,
  `PatternSong`, `TrigSelect`, `SongEnable`, `ClassicExtended`, plus the ones already there) is
  reachable by name. Verified `panel:SynthesisEffectsRouting` cycles the real parameter page and
  captured it: **AMD EQF EQG FLTF FLTW FLTQ** — exactly `HostModel`'s own per-track FX param order
  (params 8-14). The default (SYN) page's real layout is confirmed too: LEV + the current machine's
  8 SYN param names (e.g. TRXB2: PTCH DEC RAMP HOLD TICK NOIS DIRT DIST), matching
  `MachineRunner`'s per-machine descriptor table exactly — real, independent confirmation that the
  host-model translation lines up with what the actual hardware shows the user.
  - `tools/mdtrace/capture_screens.py`: drives `mdProbe` through a named list of screens (each a
    list of `panel:` actions replayed from a fresh boot, so screens don't depend on each other or
    capture order) and saves each as a PNG (a small pure-Python PGM→PNG conversion, no PIL/
    ImageMagick dependency, since neither is guaranteed available). Verified end to end: 3 screens
    (home, syn_page, amp_fx_page) captured and visually confirmed correct. Never commits its own
    output, same policy as everything else derived from the user's firmware.
  - **This is capture infrastructure, not the skin itself.** The actual generator (this project's
    `mk_skin.py` equivalent — deciding which screens the plugin needs, where on each real capture
    the touch/Q-Link regions go, and writing `TUI.json`) is real, separately-scoped work, not
    started. Also not yet done: exposing the AMP/EFX page's 9 params (only VOL/PAN are in
    `vst/engine.cpp`'s V1 param set today; AMD/AMF/EQF/EQG/FLTF/FLTW/FLTQ/SRR/DIST are wired in
    `HostModel` already but not surfaced as VST params yet) and the SYN1-8 params (trickier: their
    real names change per machine, unlike everything else).
  - **Next:** decide the skin's actual screen set and layout (a design step, best done with the
    user looking at real captures rather than guessed at), then build the compositor; separately,
    extend `vst/gen_params.py`/`engine.cpp` with the AMP/EFX and SYN1-8 params now that their real
    layout is confirmed.

- **2026-09-28: first real on-device test — deployed, played, fixed a real bug, confirmed working.**
  Deployed a quick checkpoint build (auto-generated `gen_vst.py` skin, no custom layout — dropped
  `custom_skin` from `vst.json` for this) to the Force: `.so` + the user's `.syx` (to
  `MODULE_DIR`) + skin, registered in `MPC.settings`, MPC restarted (device itself stayed up the
  whole time — `systemctl restart acvs` is not a reboot; `force_shadow.so` confirmed still loaded
  both times). The user played it and reported audible sound but choppy on some material.
  - **Root cause and fix**: the render thread was a plain `SCHED_OTHER` `std::thread` — exactly the
    kind of thing that can get starved under system load. `mpc-vst-monomodule`'s own DSP thread
    elevates to `SCHED_FIFO` priority 30 (above MPC's `AudioWorkers` at `SCHED_RR` 20) once booted,
    plus picks the least-busy non-UI core via `/proc/stat` sampling — ported both over verbatim
    (`MD_FIFO`/`MD_CPU` env overrides, matching `MNM_FIFO`/`MNM_CPU`). Also exposed a `core`
    get_param for future diagnostics (no remote way yet to query a live instance's params — worth
    a debug-log-file mechanism if a future issue needs it).
  - Rebuilt, redeployed (md5-verified), MPC restarted again. **User confirmed: sounds better.**
    Real-time scheduling was the actual missing piece for this architecture on real hardware, not
    a DSP or engine correctness issue.
  - Still on the auto-generated skin, not the bit-exact LCD one — that's the next real design/build
    step, per the entry above.

- **2026-09-28: pushed both open threads — AMP/EFX params exposed, and a real bit-exact LCD skin
  deployed and confirmed stable.**
  - **Params**: exposed AMD/AMF/EQF/EQG/FLTF/FLTW/FLTQ/SRR/DIST per track (`HostModel` raw params
    8-16, the real hardware's own AMP/EFX page order, confirmed against a captured real screen) —
    12 params/track × 16 + 2 globals = 194 total, up from 3/track. Renamed `level`→`vol` to match
    what it actually reads (param 17 VOL, not the separate kit LEV knob). Removed the hardcoded
    per-track FLTW/EQ startup defaults now that `gen_params.py`'s own declared defaults cover it.
    **Deliberately still not exposed**: SYN1-8 (their good defaults are per-machine, set by
    `HostModel::setMachine()` itself — a flat VST default would stomp them the moment a machine is
    assigned; needs "has the user touched this knob" tracking, not implemented) and
    DEL/REV/LFOS/LFOD/LFOM. Verified bit-exact: x86 smoke test peak unchanged at 5710/32767.
  - **Skin**: `vst/gen_layout.py` writes a 16-tab (+ GLOBAL) `layout.conf` — each tab shows the
    real Machinedrum's own AMP/EFX page (captured via `tools/mdtrace/capture_screens.py`, never
    committed itself) as a decorative header behind knobs for that track's 12 params, roughly
    column-aligned with the real screen's own AMD/AMF/EQF/EQG/FLTF/FLTW/FLTQ/SRR layout. Built via
    the browser art renderer (`"art": "html"` in `vst.json`, `mpc-vst-html-art` image). Previewed
    (`tools/studio.py preview`): genuinely reads as the real hardware's LCD content integrated with
    working MPC controls — not yet pixel-exact per-control alignment against the real column
    positions, but a real, working version of "the same philosophy as Monomodule."
  - **Deployed and confirmed stable on the Force**: rebuilt the `.so` (properties unchanged: one
    exported symbol, `GLIBC_2.36`), redeployed both `.so` and skin (md5-verified), MPC restarted
    (device uptime unaffected, `force_shadow.so` confirmed still loaded). Not yet re-tested by the
    user for sound/feel with the new knob layout and params — that's the natural next check.
  - **Next**: get the user's read on the new skin/params on-device; then either refine alignment
    (line knobs up exactly against the real screen's own column positions, per-tab instead of
    reusing one static capture) or move to the SYN1-8 "touched" tracking, whichever the user
    prioritizes.

- **2026-09-28: the static-whole-screen-capture skin approach doesn't scale — the user called it
  out (a single captured screen's labels are only accurate for the machine that happened to be
  showing when it was captured; the SYN page's labels are per-machine, so one static image can't
  serve all 16 tracks correctly). Agreed direction: rebuild "the Monomodule way" — reusable font/
  icon assets composited freely per screen/state, not baked screenshots. Plan below, not yet
  started; this is the resume point after compaction.**

  ## Skin plan: the Monomodule way (not started)

  The difference from Monomodule: upstream Monomodule already reverse-engineered the Monomachine's
  font/icon storage as data (`RomArt.cpp`/`SpecData.cpp`), so `mnm_artdump.cpp` just calls those
  decoders and `mk_skin.py` composites the result. We have no equivalent decoder for the
  Machinedrum's ROM. But we have something Monomodule's approach doesn't need: a full-system
  emulator (`gearmulator-md-mm`) that renders real pixels for anything we tell it to display, plus
  the bit-exact capture tooling already built and proven this session (`lcdpng`, `capture_screens.py`,
  full `panel:` navigation). The plan replaces "decode the font table" with "capture known text and
  slice it" — same end result (a reusable glyph/icon atlas), different, more tractable method for
  this project.

  **Phase 1 — Font extraction (the key unlock, do this first).**
  - Machinedrum kit/pattern/track names are user-editable ASCII strings (sysex, or
    `gearmulator-md-mm`'s automation tooling — `mdautomation.cpp`/`mdsysexautomation.cpp`, already
    referenced in "The plan" at the top of this doc for scripting parameter changes; check there
    for the exact rename mechanism first, don't assume).
  - Set kit/pattern/track names to strings covering every character the skin will ever need: A-Z,
    0-9, and whatever symbols appear in real captures (`:` `.` `-` `>` seen already in "KIT:01",
    "125.0", "TRX►B2", "PATTERN A01" — check for others once more screens are captured).
  - Capture the screen(s) showing those strings with `lcdpng` (already built and proven).
  - The font is fixed-pitch (visible in every capture so far — compare letter spacing in "TRX UW"
    vs "KIT:01" to get the exact cell pitch in pixels before writing the slicer). Slice the known
    grid into per-glyph 1-bit bitmaps at that pitch. This is a small new script (not started),
    output as a JSON atlas — same shape as Monomodule's own font dump (glyph index → bitmap rows),
    so `mk_skin.py`-equivalent code can reuse rendering logic in the same spirit.
  - Verify by re-rendering a captured string from the extracted glyphs and diffing pixel-for-pixel
    against the original capture — the gate before trusting the atlas for anything else.

  **Phase 2 — Icon assets. Done for the AMD dial (2026-09-28); same recipe covers the rest.**
  - Wrote `tools/mdtrace/build_dial_atlas.py` (committed). It reaches the AMP/EFX page
    (`panel:SynthesisEffectsRouting`) and sweeps a knob's full 0-127 range one step at a time via its
    rotary `encoder:` action (confirmed `PanelEncoder::DataEntryA` = the AMD knob - turning it also
    shows a temporary numeric readout under the dial, useful as an independent sanity check while
    developing this, though the script itself doesn't depend on reading that overlay), capturing the
    LCD after every step and deduping consecutive identical frames within a fixed crop region (found
    the same way as the font glyph band: full-frame pixel variance across the sweep, restricted to
    the dial's own area to exclude the numeric overlay and the LEV meter/neighboring dials, which vary
    too but aren't this icon). Result: **36 distinct rotation states across the AMD dial's 0-127
    range**, verified visually at 8x scale - a clean, correctly-ordered sweep of the pointer dot
    rotating clockwise around the dial. Output `dial_atlas.json` (`states`: list of
    `{value_first, value_last, rows}`) + `dial_atlas_grid.png`, both build output (gitignored).
  - **All 8 AMP/EFX dials swept (2026-09-28).** Confirmed the 8 dials sit in a uniform 4-col x 2-row
    grid by variance-scanning three of them (DataEntryA/B/E): same 14x12 icon box, offset by a
    constant pitch (`x0 = 52 + col*21`, `y0 = 14 + row*31`). Generalized `build_dial_atlas.py` to
    compute the crop region from the encoder's grid position (`dial_crop_for()`) instead of one
    hardcoded box, then ran all 8 (`DataEntryA`..`H` = AMD AMF EQF EQG / FLTF FLTW FLTQ SRR). Distinct
    icon-state counts varied a lot - `36 36 31 13 28 9 12 33` - and that's real hardware behavior, not
    a bug: spot-checking `DataEntryF` (FLTW)'s grid image showed the pointer rotating through only a
    small arc for values 0-26 and then staying pinned in the same position for the rest of 27-127 -
    i.e. this parameter's dial has a genuinely limited visual arc, confirmed by looking, not assumed.
  - **LEV bar-meter investigated, not yet solved (2026-09-28) - ruled out several easy guesses.**
    Tried: `encoder:Level:20` (no visible change to the bar at all - so `PanelEncoder::Level` is not
    what drives it), `encoder:SoundSelection:20` (this one *did* do something real - it changed the
    breadcrumb's machine name, i.e. it's the machine/sound-select knob - but still no LEV bar change),
    a trigger-and-decay sweep (`trig:1:256` then 20x `wait:1024` + `lcdpng`, variance-scanned the whole
    top-left quadrant - zero pixel variance across all 21 frames, meaning either the LEV bar genuinely
    didn't move or trigger 1 isn't the same track this AMP/EFX page is showing - the breadcrumb read
    "TRX►B2►TFX", i.e. bank B track 2, not trigger 1's track), and `panel:DataPageForward`/`Backward`
    (no visible change at all on this screen - may need a different starting screen or a hold/repeat
    semantics not yet tried). **Next things to try, not yet done**: align the triggered track with the
    displayed track (select bank/track A1 first, then trigger pad 1, before capturing the decay sweep)
    since a live VU-style meter is the most likely remaining explanation; if that still shows no
    movement, LEV may require actual DAC/output-stage audio routing this full-system emulation doesn't
    drive by default, or may be a static per-track "level" *setting* controlled by some other physical
    input not yet tried (e.g. one of the `Track1-6` panel buttons, or a dedicated hardware level pot
    modeled as its own `PanelControl`/`PanelEncoder` not yet probed).
  - Tried the "align track" idea immediately: `panel:BankA panel:Track1` before the AMP/EFX page.
    Result: `BankA` opens a "BANK A" **popup dialog** (confirmed by capture - a modal overlay with 4
    selectable squares), not a direct track-select; `Track1` after it didn't close the popup or
    change the active track (breadcrumb stayed on "TRX►B2►TFX"), and triggering while the popup is
    still open silenced playback entirely (peak dropped to 0, vs. ~0.17-0.22 without the popup open).
    So this specific combo doesn't reach bank/track selection - needs the popup's own confirm/dismiss
    sequence worked out first (probably `panel:Enter` after `Track1`, not tried yet) before this
    approach can be retried.
  - Not yet done: generalizing/verifying this same grid-formula approach on a different screen (SYN
    page knobs, which Phase 3 will also need labels for).

  **Phase 3 — UI structure (hand-authored, not extracted — the one part with no ROM-derived
  shortcut). Started 2026-09-28.**
  - Wrote `tools/mdtrace/ui_spec.py` (committed): the SYN page and the AMP/EFX page turned out to be
    **the same physical 4x2 dial-grid widget** - confirmed by diffing the two captures, dial positions
    are pixel-identical, only the label text and bound parameter differ. `DIAL_GRID` captures the one
    shared geometry (dial crop per cell, label text band at y=3-7 above each dial); `SCREENS["amp_fx"]`
    and `SCREENS["syn"]` each just reference it with their own label source.
  - **Independently verified the "SYN1-8 labels are free from MachineRunner" claim against real
    hardware ground truth** (not just trusted from reading the header comment): built a standalone
    `mdmachine` tool (compiled `tools/mdmachine/mdmachine.cpp` directly against the existing
    `build-vst-x86/libmdcore.a` + `mc68k`/`dsp56kEmu` static libs with a plain `g++` invocation -
    faster than a full CMake reconfigure for a one-off tool check) and ran it against the user's OS
    `.syx`. It lists every machine's name and 8 SYN param names, decoded from the OS's own machine
    descriptor table. Machine 28, `TRXB2`, reports `PTCH DEC RAMP HOLD TICK NOIS DIRT DIST` - an
    **exact match** to the SYN page capture from earlier this session (same 8 labels, same order) -
    and `TRXB2` is exactly what the real LCD's breadcrumb showed split across two segments
    (`TRX►B2►SYNT`). So `MachineInfo::params` is confirmed correct and sufficient for Phase 3/4's
    per-machine SYN labels - no manual transcription needed, and now proven, not just assumed.
  - Not yet done: pixel-verifying the label text band's column boundaries per-column (currently an
    approximate `label_col_pitch=20`, not confirmed against a third page); confirming whether any
    screens exist beyond SYN/AMP-EFX (LFO page? routing page? - `panel:DataPageForward/Backward` had
    no visible effect when tried from the AMP/EFX page, logged under the parked LEV investigation).

  **Phase 4 — Compositor. First working version 2026-09-28.**
  - Wrote `tools/mdtrace/compose_skin.py` (committed): given the font atlas, one dial-pointer atlas
    per knob, and `ui_spec.py`'s `DIAL_GRID`, it draws a screen's 8 dial icons + labels for a chosen
    set of live values into a PNG. Ran it for the AMP/EFX screen (all values 0): **the 8 dial icons
    composited correctly** - right shape, right position, aligned with the grid's divider ticks, a
    real structural proof that the font/icon atlases and the hand-authored grid geometry all agree
    with each other.
  - **Found a real gap while looking at the result, not by inspection alone: the header label text
    overlaps between columns** (e.g. "AMD"/"AMF" run together as "AMDAMF" with no gap). Measured why:
    the label text band is `y=3-7` (5px tall), but `build_font_atlas.py`'s font (extracted from the
    kit-rename "Enter name:" screen) is `13px` tall - **these are two different fonts at two different
    sizes**, not one font reused. The name-entry font's glyphs are simply too wide for the ~20px label
    columns here. Reusing the name-entry atlas for header labels was an unverified assumption in the
    original Phase 1/3 plan text (which only ever explicitly needed the name-entry font for kit/track/
    pattern *names*, not dial labels) - now corrected.
  - Not yet done, and the concrete next step: extract a **second, smaller font atlas** for this label
    band, using the same diff-across-a-capture-sweep technique as `build_font_atlas.py` but sourced
    from the AMP/EFX and SYN page label rows themselves (their content varies with which machine is
    assigned - Phase 3 already showed `MachineInfo::params` gives real, varied label text for free, so
    swapping machines and diffing the label band across several different machines' SYN pages should
    reveal this small font's glyphs the same way the name-entry sweep did for the large one). Once
    that exists, `compose_skin.py` should render clean, non-overlapping labels; the dial-icon half of
    the compositor is already working and shouldn't need changes.

  **Scope note**: this is genuinely comparable in size to Monomodule's own `mk_skin.py` (~800
  lines) plus the font/icon extraction Monomodule got for free from upstream and we don't have —
  a multi-session build. All four phases now have a working first pass; the small-label-font
  extraction above is the most concrete remaining gap before the compositor is presentation-ready.

  **Phase 1 progress (2026-09-28): rename mechanism confirmed empirically, unblocks capture.**
  - No sysex/data API sets a kit/pattern/track name (checked `mdautomation.cpp`/`mdsysexautomation.cpp`/
    `mdrom.cpp`/`mdromdata.cpp`/`mdflash.cpp`/`mdsim.cpp` — none). Confirmed real hardware behavior:
    names are only editable through the front panel's own character-picker screen.
  - Added an `encoder:NAME[:STEPS]` action to `tools/mdtrace/mdProbe.cpp` (mirrors
    `mdEditor.cpp`'s `Editor::emitEncoderSteps`: `panelEncoderCommand()` + `sendPanelEvent(cmd, 0x01/0xff)`
    per detent) since `mdpanel.h`'s `PanelEncoder` (DataEntryA-H/Level/SoundSelection) looked like the
    likely input for a character picker. **It isn't** — rotating any `DataEntryA` steps had no effect on
    the name-entry screen in testing.
  - What actually works, found by probing live (`mdProbe` + `lcdpng`, converted to PNG and inspected):
    `panel:Kit` → popup with LOAD/SAVE/EDIT/MASTER quadrants → `panel:Right` selects SAVE →
    `panel:Enter` → save-slot list (`1-USERKIT`, `2-EFR UW`, ...) → `panel:Enter` on a slot → an
    "Enter name:" screen with the name on one line and a `«  »` cursor indicator below the selected
    character. From there: **`panel:Up`/`panel:Down` cycles the character at the cursor** (steps by
    ASCII-ish order; a 2048-frame hold auto-repeats ~2 steps — use a short `hold` argument, e.g.
    `panel:Up:128`, for single-character control), **`panel:Left`/`panel:Right` moves the cursor**
    between name positions, `panel:Enter` confirms/saves. Copy of the exact working action sequence
    (from a fresh boot, ROM path is the user's own `elektron_sps1-1uw_os1.63.bin`, not committed):
    `panel:Kit panel:Right panel:Enter panel:Enter panel:Right:128 panel:Up:128 ... panel:Enter`.
  - **Single-step confirmed directly (2026-09-28), correcting an initial misread**: a sequence of six
    `panel:Up:64` calls in one boot, each followed by an `lcdpng` capture, was inspected at 6x
    upscale (small thumbnails were ambiguous - don't trust them for glyph work, always upscale before
    reading) and stepped exactly one character per tap: `T -> U -> V -> ...` Each `panel:Up`/`panel:Down`
    with `hold=64` is a clean, reliable single-character step; no auto-repeat contamination at that
    hold length. This means a sweep script can walk the full character set deterministically by
    counting taps from a known start character - no need to guess distances or verify by trial capture
    each time.
  - **Full charset order walked and captured (2026-09-28)**: 45 sequential `panel:Up:64` taps from
    a start char of "T" were each captured with `lcdpng`, cropped to the name-entry line, and
    stacked into one strip image for direct reading (a much better technique than one-off captures -
    reuse this "stack N frames into a vertical strip, read once" trick for any future sweep, it beats
    inspecting frames one at a time). Confirmed order, continuing forward from T:
    `T U V W X Y Z [2 accented/special glyphs, unconfirmed exact chars - look like "A-ring" and
    a second variant] Ø <space> 0 1 2 3 4 5 6 7 8 9 + - = [Ø again, or a lookalike - verify] /
    ( ) , ! ? A B C D E F G H I J K L M N O` (then presumably continues P Q R S T, wrapping). So the
    cycle is essentially: **A-Z, a couple of accented/special characters, Ø, space, 0-9, a small
    symbol set (`+ - = Ø / ( ) , ! ?`), then wraps to A** - i.e. everything needed for the skin
    (A-Z, 0-9, space, `: . - ►` etc. seen in real captures) is reachable, though the exact symbol
    for `:` and `►` wasn't hit in this 45-tap window and needs a longer walk or starting from a
    different point to confirm their position/glyph shape.
  - **Font atlas built and verified (2026-09-28) - Phase 1's core deliverable is done.** Wrote
    `tools/mdtrace/build_font_atlas.py` (committed - it's code, not firmware-derived data). It drives
    the full corrected 50-entry cycle from a fixed start point (steps to `SPACE` first via the known
    offset from the default "T", then walks forward capturing all 50), and **autocrops the glyph cell
    by pixel-diffing across all 50 frames** (the cell that changes frame-to-frame *is* the glyph, no
    manual pitch measurement needed - this technique generalizes to Phase 2's icon sweep too). Output:
    `font_atlas.json` (`glyph_w`/`glyph_h`/`band_x`/`band_y` + a `glyphs` dict keyed by name, each a
    list of `"01..."` bit-row strings) plus a `font_atlas_grid.png` for visual sanity-checking - both
    build output, gitignored (`build*/` already covers it; ran the script into a scratch dir this
    session, not `vst/build/`, so nothing new needed there).
  - **Found and fixed a real off-by-10 labeling bug the same session it was introduced.** The first
    working version's preloop (to walk from the default kit name's first char "T" back to `SPACE`
    before starting the sweep) tapped `Up` `CYCLE.index("T")` (= 40) times instead of the needed
    `(0 - 40) % 50 = 10` times, landing 30 slots past `SPACE` - so every captured glyph was stored
    under a label 10 slots away from its true character (e.g. the bitmap stored as `"A"` was actually
    `PLUS`'s glyph). **This was invisible in an eyeballed grid image** - relabeling-then-redisplaying
    glyphs by their (wrong) stored key still produces a self-consistent-looking, alphabetically-ordered
    grid, because the display script trusted the same wrong labels. It only surfaced by dumping a few
    specific letters' raw bitmaps as ASCII (`A` looked like a plus sign, not a letter A) and checking
    their *shape* against what that letter should actually look like. **Lesson for any future
    label-from-position capture work**: verify a labeled asset against an independent ground truth of
    what the label should look like, not just against a re-rendering of itself. Fixed: preloop now taps
    `(-CYCLE.index("T")) % 50` times, and the per-frame label lookup no longer double-counts the
    preloop offset. Re-ran and re-verified: `T`, `U`, `I`, `O`, `S`, `L` all now show correct,
    recognizable letterforms; the natural capture-order grid (`font_atlas_grid.png`) also reads
    correctly as `0-9, + - = Ø / ( ) , ! ?, A-Z, Å Ä Ø` without any relabeling.
  - Corrected an earlier misreading in this same log: the cycle is exactly 50 entries, not 51 - what
    an earlier partial 45-tap walk logged as a trailing "PERIOD" was actually just the wrap back to
    `SPACE` (which looks blank, hence the confusion). `CYCLE` in `build_font_atlas.py` is now the
    checked-good ordered list.
  - **The font is proportional, not fixed-pitch** - this corrects the original Phase 1 plan text's
    assumption ("The font is fixed-pitch... compare letter spacing to get the exact cell pitch").
    Swept the cursor across 8 name positions (`panel:Right:64` x8) with the fixed suffix "RX UW"
    showing and diffed consecutive frames: the selection box's position deltas were `8,6,6,5,6,9,5`
    px - not constant, meaning each glyph's natural width varies, not a fixed grid.
  - **Per-glyph `advance` width now measured reliably.** The first attempt's `ink_x0`/`ink_x1`/
    `advance` were contaminated by the cursor box's own dashed corner ticks (present in every glyph's
    crop, touching column 0 and the right edge) and reported every glyph as full-width. Fix: intersect
    all 50 captured glyph bitmaps to find pixels that are ink in literally every one - this isolated
    the ticks to rows 0-2 and 11-12 of the 13-row crop band, with the actual character body confined
    to rows 3-10 (`GLYPH_BODY_ROWS`). Restricting ink-column trimming to that row range gives sane,
    varied results: `SPACE` advance 2px, most letters 6-7px, none pinned at the full 8px band width
    anymore. This is real, physically-measured per-glyph spacing, ready for Phase 3/4 text layout.

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
