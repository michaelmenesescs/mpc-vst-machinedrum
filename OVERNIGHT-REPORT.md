# Overnight report: JIT warm-up for Machinemodule's MPC plugin

Branch `perf/jit-prewarm` (off `main` v0.5.1, `de3731e`), pushed to `michaelmenesescs/mpc-vst-machinedrum`. 2026-10-29.

**Result.** The plugin now warms the voice DSP's JIT on its loader thread before it reports `ready`. The audio is unchanged,
bit for bit (md-hash, plus a new warm-vs-cold sample comparison over 23k-65k blocks). In the aarch64 container the startup
underrun burst is gone (20-24 -> 0-3), and machine-change underruns are down to the floor of this environment under a
16-voice load. Load time goes up by about 0.24 s and physical memory by about 70 MB. Nothing was run on a device.

## Provenance

- `VoiceEngine::saveState` / `restoreState` / `warmUp` are adapted from **c0remusic/MModule** (GPL-3.0),
  `source/elektron/md/mdEngine/engine/VoiceEngine.cpp:667-718` (State: P/X/Y + DSP registers + bookkeeping).
- `EngineT::warmUpVoices` is adapted from **MModule** `MdEngine.h:64-97`: a Hit at default SYN1-8, 16 machines per pass over
  the 16 slots, words from a copy of the OS, then restore, returning the machines run or -1.
- The test method (warm and cold engines on one script, compared sample for sample, with a timed first hit) comes from
  **MModule** `mdDrumsFirstHitTest.cpp`.
- The limits stated in **AcidReff/MachineModule** `README.md:105` (parameter-dependent code never seen, ROM paths) were
  checked here and are covered below.
- Attribution is in the code comments of `engine/VoiceEngine.cpp`, `engine/MdEngine.h` and `tools/mdwarm/mdfirsthit.cpp`.
  The mechanism was ported, not the files.

## What changed (and why it differs from MModule)

| file | change |
|---|---|
| `engine/VoiceEngine.{h,cpp}` | `State`, `saveState()`, `restoreState()`, `warmUp(script)`, `hasJit()`. md-perf had only `reset()`, which reloads the program and so throws away the JIT's code. |
| `engine/ParallelVoiceEngine.{h,cpp}` | `warmUp(script)`: **every group** runs the whole script with all 16 of its own slots, one thread per group, all at once. Stats: per group, the slowest block's wall time, CPU time and DSP instructions. |
| `engine/MachineRunner.{h,cpp}` | `clone()`: slot words are computed on a copy of the OS, so the engine's own 68k state is never touched. |
| `engine/MdEngine.h` | `warmableMachines(rom)` and `warmUpVoices(machines, blocks)`. |
| `vst/engine.cpp` | Warm-up in `Inst::run` after `loadSamples`, before thread tuning and before `ready`: every offered machine, with the ROM machines included only when the samples are loaded. 32 blocks each. `MD_WARM` and `/tmp/md-warm` override it (0 = off). Skipped where there is no JIT (Gen1 armhf). |
| `tools/mdwarm/mdfirsthit.cpp` (new `md-firsthit`) | Warm vs cold engine: per-machine first-hit cost, sample identity, the smoke's churn script replayed offline (`MD_FH_CHURN`), a SYN sweep (`MD_FH_PARAMS`), and RSS per machine (`MD_FH_RSSSTEP`). |
| `tools/mdhash/mdhash.cpp` | `MD_WARM=n` (warm first; the hash must not change) and `MD_RSS`. |
| `vst/smoke.cpp` | Prints `first audio after N ms`. |

Details that differ from MModule, and why:
1. **The P layout here is 8M words (32 MB) per DSP**, because the bridged external memory is sized like X/Y.
   `saveState` keeps P only up to its last non-zero word, so the saved state is about 7.5 MB per group.
2. **The JIT is told about every restored word.** `restoreState` writes P directly, as the DSP's own data writes do, and
   reports any changed word inside the range `reset()` loaded through `memWriteP` to the JIT. That way no block compiled from
   a word that changed meanwhile can survive. Measured: 9,216 words differ at restore, all in `$135600-$13b5ff` (voice state
   inside the program's range, i.e. data). No code word changes, so every compiled block stays valid.
3. **32 blocks per machine, not MModule's 4.** With 4, TRX BD's code reached only after its 4th block stayed cold: 9-11 ms on
   that machine's first hit, warm (Mac and aarch64). 16 and above remove it. Warm-up time is almost flat in the block count
   (aarch64, 3 groups, ROM: 249 / 231 / 242 ms for 4 / 16 / 32), because compiling dominates.
4. **ParallelVoiceEngine: every group warms every machine, all groups at once.** This was the brief's main worry. All groups
   together take 245 ms against 160 ms for one group, so no chunking or lazy warming was needed. It is also *required*:
   `HostModel::trigger` moves a re-triggered voice to the least-loaded group, and a cold group compiles that machine again.
   With no machine changes at all, a cold engine still had 27-46 underruns in 30 s for this reason (see below).

## Gates

- **md-hash** (both macOS arm64 native and aarch64 container), main vs branch, warm-up off and on (`MD_WARM=32`):
  - without ROM samples: `04ef1db4fe767721` (1 and 3 groups) in every case;
  - with ROM samples: `e2b514e70c173c33` (1 and 3 groups) in every case. These are the documented reference hashes.
- **md-firsthit** (warm and cold engine on the same script; main, sends, the 6 individual outputs and all 16 tracks
  compared): 0 differing blocks.
  - 23,020 blocks: every machine's first hit, plus a 2,000-block tail.
  - 65,020 blocks with the SYN sweep.
  - 23,000+ blocks in the churn replay.
  - Mac and aarch64, 1/2/3 groups.
- **md-vst-smoke**, base vs branch: every deterministic line is identical.
  - Lines compared: SYN defaults and labels, touched-knob handling, LFO labels, factory kit loads, kit/bank steppers.
  - Fixed kit `MD_SMOKE_KIT=16,17,22,28,32,49,64,130`: peak 29081 in both. The `randomize_9_16` line differs between any two
    runs (seeded from the clock).
  - Sample-level identity of the *plugin's* stream can't be tested: notes reach the engine thread asynchronously, so it isn't
    deterministic in either build. Engine-level identity is what md-hash and md-firsthit prove.
- **Starting sound:** the warm-up runs before the first render and puts the state back exactly, as the md-hash/md-firsthit
  identity shows. A project restore or kit load lands on the restored engine the same way it did before.

## Measurements

Environment: Mac mini M-series, 10 cores. aarch64 numbers come from the `md-aarch64-builder` container (OrbStack, native
arm64, 8 GB VM). The container cannot grant SCHED_FIFO, and its cores are much faster than a Live III's, so absolute numbers
are lower than on a device. Data dirs: the OS `.syx`, with or without `factory/` (FACTORY.syx + ROM_SAMPLES.bin made from
the flash image by `make_factory.py`; md5 72487acb… / 630e7baf…, the reference ones). All outside the repo.

### Smoke, `MD_SMOKE_CHURN=1`, 30 s, aarch64

All 16 tracks hit every 16th, and a random track gets a random machine (16-127) every 0.5 s, 56 changes after t = 2 s.

| build | ready (load) | first audio¹ | underruns t ≤ 2 s (startup) | underruns t 2-30 s (56 changes) |
|---|---|---|---|---|
| main, no ROM (3 runs) | 141-169 ms | 1773-1811 ms | 23, 23, 24 | 44, 50, 44 (≈0.8 per change) |
| main, ROM (3 runs) | 136-158 ms | 1770-1785 ms | 20, 23, 24 | 42, 49, 51 |
| branch `MD_WARM=0`, ROM (3 runs: same binary, warm-up off) | 124-137 ms | 1753-1769 ms | 22, 23, 25 | 38, 39, 43 |
| **branch, no ROM (final build, 3 runs)** | 361-380 ms | 1928-1955 ms | 1, 1, 3 | 9, 9, 8 (≈0.15 per change) |
| **branch, ROM (final build, 3 runs)** | 377-400 ms | 1955-1978 ms | 0, 0, 0 | 5, 8, 5 (≈0.1 per change) |

¹ From `create()`. The smoke itself waits 1.5 s for the kit catalog before it renders, so first audio is ready + ~1.55 s
in both builds. The warm-up delays it only by its own duration.

- **Load time added:** the warm-up is 220-249 ms in the plugin, with 57 machines (no ROM) or 105 (ROM) × 3 groups. `ready`
  moves from about 145 ms to about 380 ms.
- **Control, the same 16-track load with no machine changes** (`MD_SMOKE_KIT` = churn's 16 starting machines, 30 s):
  - main: 27 and 46 underruns;
  - branch: 6 and 4.

  So with the warm-up, the churn runs' 5-9 underruns are this container's floor under a 16-voice load, not machine changes.

### Per-second plugin stats (`MD_STATS`, worst 128-frame host block, 2.9 ms deadline), churn, ROM

- **main / `MD_WARM=0`:**
  - 63-65 ms in the first second;
  - then 13-16 seconds per run over 4 ms, up to 21 ms.
  - In the slow block, one voice group's CPU time equals its wall time while it runs a normal DSP instruction count
    (6-9k): CPU spent outside DSP execution, i.e. compiling.
- **branch:**
  - 0-2 seconds per run over 4 ms (4.5-9.5 ms).
  - Two kinds were seen, and neither looks like a compile:
    - all three groups slow by the same ~5 ms in the same second, which a compile cannot cause (one machine compiles in
      one group). It looks like a VM stall, which a guest also counts as thread CPU;
    - the time spent outside the voice groups entirely (every group's worst block < 0.9 ms).

### First hit, warm vs cold engine (md-firsthit, 3 groups; slowest block within 200 blocks after the hit)

| | cold mean | cold max | cold hits > 2.9 ms | warm mean | warm max | warm hits > 2.9 ms |
|---|---|---|---|---|---|---|
| macOS arm64, 105 machines, 32 warm blocks | 5.6 ms | 43.7 ms | 29 | 0.15 ms | 0.23 ms | 0 |
| aarch64 container, 105 machines, 32 warm blocks | 2.0 ms | 10.2 ms | 25 | 0.20 ms | 0.36 ms | 0 |
| aarch64, 4 warm blocks (MModule's) | 2.1 ms | 10.9 ms | 25 | 0.23 ms | 2.9 ms (machine not printed; on macOS it is TRX BD) | 0 |

- The worst machines cold are TRX SD/CB/OH, P-I BD/SD/MT and GND SN/NS: 20-54 ms on macOS, up to 10 ms on aarch64.
- **Churn replay offline** (the smoke's script, deterministic, aarch64), blocks over 725 µs (one 32-sample block):
  - cold: 25-31;
  - warm: 0-3, not reproducible. The one inspected was 2.0 ms on a change where the cold engine took 0.2 ms: preemption.

### Residual cost (task step 5)

- **Parameter-dependent code (SYN1-8):** none found.
  - After the default warm-up, every machine was replayed at SYN all 0, all 127 and 3 random sets.
  - Warm blocks over 725 µs: 0, 0, 0, 1 (E12CC at 732 µs, noise level), 0. Max 0.73 ms.
- **ROM machines:** warmed with their samples loaded, so this is not a limit here, unlike in MachineModule. When the samples
  are missing they are skipped, because the plugin swaps them to GND-- anyway.
- **Not covered by the warm-up:**
  - LFO-modulated slot words (the same code with other values, so presumably like the SYN sweep);
  - MID/CTR machines (not sent to the DSP).
  - The OS 1.63 table has no RAM-R/RAM-P machines at all (the skip is kept for parity with MModule).
  - Gen1 armhf: no JIT, warm-up skipped (`VoiceEngine::hasJit`); not built or tested here.

### Memory (important when reading RSS)

- **VmRSS jumps from ~2.3 GB to ~5.0 GB with the warm-up, but this is not physical memory.**
  - The JIT keeps one block chain per DSP mode: the addressing mode of all 8 `m` registers plus the SR mode bits, from
    `JitDspMode`.
  - Each chain's per-PC tables are `MmuArray`s spanning the program (~1.34M entries, 48 MB of address space).
  - On Linux, every block of such an array is first mapped to *one shared default block*. RSS (`RssShmem`) counts every
    mapping of that one block, so it inflates by 48 MB per chain however little is resident.
  - Measured with `MD_FH_RSSSTEP`: steps of exactly 48/96/144/192 MB per machine, 27 chains per DSP once all machines have
    run. A cold engine reaches the same RSS after playing every machine once (+1.30 GB per DSP either way).
- **Physical memory** (the VM's `/proc/meminfo`: Shmem and MemAvailable, counted once):

  | | at 10 s | at 30 s of churn |
  |---|---|---|
  | main | +150 MB shm, −428 MB available | +169 MB shm, −420 MB |
  | branch | +223 MB shm, −503 MB available | +223 MB shm, −489 MB |

  So the warm-up costs about **+55-75 MB of physical memory** at load, which main also grows toward as machines get played.
- The saved state (≤ 7.5 MB per group) is freed after the restore.
- **Watch on a device:** if a tool or the MPC's memory accounting goes by VmRSS, the plugin will look like 5 GB.

## Failures and gotchas along the way

- `docker build` failed twice on `apt` "Hash Sum mismatch" (different packages each time), which looks like downloads being
  corrupted on this network. I built `md-aarch64-builder` from an identical Dockerfile in `/tmp` with apt pipelining off
  (`Acquire::http::Pipeline-Depth "0"`, `No-Cache`, `Retries`). The repo's Dockerfile is unchanged; consider adding that line.
- Docker's credential helper (`docker-credential-osxkeychain`) isn't on PATH for non-interactive shells. I used
  `DOCKER_CONFIG=~/.docker-nocreds` (an empty config) for `docker build` / `build_so.sh`.
- This checkout's `origin` is `sd88me/mpc-vst-machinedrum` (the push was refused with 403; nothing went out). I pushed to
  `https://github.com/michaelmenesescs/mpc-vst-machinedrum.git` directly and left the remote config unchanged.
- `tools/mdtrace/build_mdprobe.sh` patched `libs/gearmulator-md-mm`'s working tree, as it always does, to build
  mdProbe/mdExtract for the ROM samples. The submodule shows as modified (`m`). It was not committed and must not be.
- No mpc-vst-plugins checkout existed; I cloned `sd88me/mpc-vst-plugins` (depth 1) to `~/Coding/mpc-vst-plugins` for the
  wrapper and `gen_vst.py`.

## Reproduce

```sh
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build-mac md-hash md-firsthit
MD_GROUPS=3 MD_WARM=32 build-mac/md-hash <OS.syx> [ROM_SAMPLES.bin]          # hash must equal the warm-up-off one
MD_FH_BLOCKS=200 MD_WARM_BLOCKS=32 build-mac/md-firsthit <OS.syx> [ROM]       # per-machine first hit, identity
MD_FH_CHURN=30 build-mac/md-firsthit <OS.syx> [ROM]                           # the smoke's churn offline
MD_FH_PARAMS=1 build-mac/md-firsthit <OS.syx> [ROM]                           # parameter-dependent code
vst/build_so.sh - <mpc-vst-plugins> aarch64                                    # then in the container:
docker run --rm -v $PWD/vst/build/obj-aarch64:/b -v <data>:/w md-aarch64-builder sh -c 'MD_SMOKE_CHURN=1 /b/md-vst-smoke /w/<dir> 30'
```
Scratch scripts used for the tables (not committed): `~/mdwork/{hash,smoke,ab,slow,rss,phys}.sh` and `summ.py`.

## Next steps

1. **On a Live III:** read the log line `[machinedrum] JIT warm-up: N machines x 3 groups, 32 blocks each, T ms` for the real
   load cost (guess: 0.5-1 s; the device cores are slower). Then compare `MD_SMOKE_CHURN=1` underruns with
   `/tmp/md-warm` = 0 and absent (HANDOFF: 33-76 at startup, 20-30 per change before).
2. If the device's load time matters: warm the current kit's machines first, report `ready`, and warm the rest on the
   loader thread before the first render. This needs a way to pause rendering, so for now I'd keep it simple (a single
   pass before `ready`).
3. If VmRSS-based accounting turns out to matter on the MPC: the 48 MB-per-mode-chain tables are a `libs/dsp56300` matter
   (sparse tables, or sharing chains across modes). That's out of scope here and would need a submodule bump.
4. Optionally add the apt pipelining line to `tools/Dockerfile.aarch64-builder`.
