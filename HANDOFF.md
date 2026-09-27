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
