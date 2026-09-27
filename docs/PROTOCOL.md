# CPU-to-DSP protocol: findings from tracing gearmulator-md-mm

Captured with `tools/mdtrace` against `elektron_sps1-1uw_os1.63.bin` (the same image
`docs/FIRMWARE.md` decodes from the `.syx`). 5 emulated seconds, `mdTraceTool ROM.bin 0 220500`.

## DSP roles: confirmed

`FIRMWARE.md` guessed the roles from section size. Tracing settles it: the actual ColdFire→DSP
program transfer was captured and matched **word for word** against the two sections decoded by
`tools/mdfw` from the `.syx`.

| DSP (gearmulator-md-mm) | HI08 window | OS file section | Role |
|---|---|---|---|
| DSP1, index 0 (`m_dspMixer`) | `$500000` | **section 2** (small, 18,823 words) | mixer / codec / master FX |
| DSP2, index 1 (`m_dspProducer`) | `$600000` | **section 1** (large, 250,123 words) | voice producer |

This confirms the split the user wants: **Machinedrum One** (the voice machines) is section 1 on
DSP2; **Machinedrum FX** (the master effects, as a separate plugin) is section 2 on DSP1. It also
confirms `tools/mdfw`'s decoding is structurally and byte-exactly correct: the ColdFire firmware's
own program transfer carries precisely the record stream the `.syx` decodes to.

## How the transfer actually works: two stages

The wire protocol is not "one record stream over the standard dsp56300 boot protocol" as first
guessed. It's two stages:

1. **A tiny first-stage loader**, uploaded via dsp56300's ordinary `DspBoot` Length/Address/Data
   host-port protocol (`BOOT dspN word=...` in the trace): 153 words to DSP1 at P:$100, ~184 words
   to DSP2. This is what the `mddsp.cpp` comment calls "the Machinedrum second-stage loader" — the
   thing that "uploads its main program into external SRAM and jumps there". `onDspBootFinished()`
   fires as soon as *this* tiny upload completes; gearmulator-md-mm's own scheduler considers the
   DSP "booted" at that point, well before the real program has arrived.
2. **The real program**, streamed in afterward as ordinary runtime host-port words (`RUN dspN
   UC->DSP` in the trace) — read by the first-stage loader now running *on* the DSP, not by any
   C++-side boot state machine. This is the record stream `tools/mdfw` decodes: `[3][$24]`
   (unconsumed by the wire — it configures the loader before transfer, not sent) then, exactly as
   the file has it, back-to-back `[space][addr][count]` headers each followed by `count` data
   words. The trace matched the section byte-for-byte from the first data word (`$24`, the shared
   vector-table load address both programs start at) for as far as compared (see below).

Once a DSP's record stream is exhausted, the traffic changes character: both DSPs finish loading and
reach runtime operation, and the rest of the traffic is the runtime protocol below.

## Runtime protocol (decoded)

Captured with `tools/mdtrace` (`mdProbe`) on a properly booted machine: first-run flash
initialisation, then 20 emulated seconds of `advance()`, then scripted triggers, MIDI CCs and sysex.
The DMA log records every word the host writes into DSP memory, so the tables below are exact, not
inferred from the host-side word order.

### Host commands: the ColdFire just writes DSP memory

Both DSPs expose the same small command set on their HI08 port (`vba` = vector address):

| Vector | DSP2 handler | DSP1 handler | Does |
|---|---|---|---|
| `P:$12` | `$e8` | `$9aa` | **Block write.** Handler reads a destination address and a count from HRX, then programs DMA channel 5 (source HORX, request = host receive, destination space **Y**) to copy `count+1` words from the host port into `Y:dest`. |
| `P:$10` | `$f4` | `$9b6` | **Peek.** Reads an address, replies with one word (DSP2 `X:(addr)`, DSP1 `Y:(addr)`). The ColdFire polls a few DSP1 words (`Y:$1be/$1bf`, meters) this way. |
| `P:$14` | `$fd` | — | Batched table lookup (DSP2 only; not seen at runtime). |

So there is no "parameter block protocol" as such: the ColdFire keeps a set of fixed-layout structures
in each DSP's Y memory up to date, and the DSP code reads them. Our host model can do the same by
writing DSP memory directly — no HI08 emulation needed.

### Update rate

Everything runs on a fixed control tick of **96 samples** (2.18 ms): DSP1's structures are rewritten
every tick, DSP2's voice slots every other tick (192 samples).

### DSP2 (voice producer): 16 voice slots

`Y:$800 + $40·k` for voice k = 0..15 (track 1..16), 64 words each; the host writes the first 13.

| Word | Content |
|---|---|
| 0 | **Trigger/machine code.** Non-zero only on the tick a voice is triggered: machine index + 1, with bit 7 set for the UW (ROM/RAM) machine table (UW machine 16 → `$91`, 32 → `$a1`; classic-table default kit tracks gave `$1d`, `$12`). 0 on every other tick. |
| 1..12 | **Machine coefficients**, precomputed by the ColdFire from the track's SYN1-8 parameters (plus LFO modulation). Re-sent every tick while the voice is active; idle voices only get words 0-1 = 0. |

Parameter sweep on track 1 (TRX default kit machine), CC 16-23 = SYN1-8:

| Param | Slot words changed |
|---|---|
| SYN1 | 1 |
| SYN2 | 3 |
| SYN3 | 2 |
| SYN4 | 4 |
| SYN5 | 12 |
| SYN6 | 11 |
| SYN7 | 9, 10 |
| SYN8 | 5, 6, 7, 8 |
| LFOD, LFOM (LFO depth/mode) | 1 (the LFO modulates word 1 over time) |

The words are machine-specific coefficients, not the raw 0-127 values (e.g. SYN6 = 20 → `$140000`,
100 → `$640000` is linear, but SYN8 changes four filter-style coefficients at once). **Computing
these 12 words per machine is the core of the host model** — see "Open: coefficient generation".

### DSP2 → DSP1: 16 separate voice streams

The ESSI0 link carries 16 words per sample period, as 512-word superframes every 32 samples: **one
32-sample mono block per voice**, voice k at block k (inactive voices send zeros). Triggering tracks
1, 2 and 5 puts the audio in blocks exactly 1 and 4 apart. DSP2 does **not** mix: per-voice audio is
fully separable at this link.

### DSP1 (mixer): per-track effect chain, mix and master FX

| Y address | Per | Content |
|---|---|---|
| `$200 + $40·k`, words 0-8 | track k | **AMD, AMF, EQF, EQG, FLTF, FLTW, FLTQ, SRR, DIST**, each the raw 0-127 value `<< 7` |
| `$100 + 5·k`, words 0-4 | track k | word 1 VOL (a computed gain), 2 PAN (`value << 16`), 3 REV send, 4 DEL send |
| `$150-$158`, `$170-$18c` | global | change every tick (master FX / modulation state); not decoded |

So the whole **per-track effects page (amp modulation, EQ, filter, sample-rate reduction,
distortion) runs on DSP1**, together with the volume/pan mix and the master effects. DSP2 only
produces the raw machine voices.

### DSP load

Measured with `mdProbe prof` (PC histogram + instruction counters):

| DSP | Idle | 16 voices / pattern playing |
|---|---|---|
| DSP2 (voices) | spins in a delay loop at `P:$100090` (~96% of samples): inactive voices cost ~nothing | ~45% in the link-DMA wait at `P:$cf` → roughly **35-60 M instructions/s** of work |
| DSP1 (per-track FX, mix, master FX) | no wait loop in its hot spots: filter/MAC loops over 32-sample blocks | same: **~79 M instructions/s, always** |

Monomodule's one Monomachine track is ~21 M instructions/s and needed the static recompiler to fit
58% of one Force core. Emulating both MD DSPs is roughly 5-7× that.

## Host model: the ColdFire side (decoded)

Found by watching the ColdFire (68k) code that writes DSP2's host port (`tools/mdtrace`: `cf:on`,
`watch:`), then reading the OS image (section 0 of the `.syx`, load base `$200000`; the running RAM
matches the file byte for byte) with Capstone (`tools/mdtrace/analysis/dis68.py`).

- **Pump.** An interrupt handler in the ColdFire's internal SRAM (`$1000760-$100077c`) streams a
  staging buffer of 32-bit words (`$010015b4 + …` per voice) to DSP2's HI08. It only transports.
- **Machine descriptor table** at `$24ef54`: 135 records of 86 bytes = `[coefficient function
  pointer][machine id][5-char name][8 × 4-char parameter names][8 defaults][flags…]`. It covers
  every MD UW machine: GND, TRX, EFM, E12, P-I, INP, MID, CTR, ROM, RAM
  (`tools/mdtrace/analysis/machines.py` lists it from your own OS file). MID/CTR point at a stub;
  ROM/RAM machines share two functions.
- **Machine coefficient functions are pure.** Each is compiled C, called as
  `count = fn(uint32_t *out, const uint16_t *params)`: it reads the 8 SYN parameters as 16-bit
  values, computes `out[1..12]` with arithmetic and lookup tables that live in the OS image, and
  returns the word count. No other OS state. Example: TRX-B2 (`$201fd2`) is ~70 straight-line
  instructions. **So Machinedrum One can call the original functions in a 68k emulator (Musashi,
  already in md-mm) for exact results at negligible cost** (worst case 16 voices × every 192-sample
  tick ≈ 3,700 calls/s ≈ 0.3 M 68k instructions/s).
- **Caller** (`$20b394`, inside the per-voice tick routine): `jsr` through a per-voice function
  pointer array in RAM (`$29f27c`), passing the voice's staging slot and `a6`. **`a6` is the track's
  current parameter array: 24 16-bit values** (`$0-$e` SYN1-8, `$10-$20` AMD..DIST, `$22-$28`
  VOL/PAN/sends, …), i.e. the kit values after the OS's own modulation. The same routine turns the
  non-SYN part into DSP1's per-track values (with velocity/accent handling).

### The per-track parameter pipeline (decoded)

`a6` for voice k is `$010011cc + $30·k` in internal SRAM (24 16-bit values). It is produced each
**sequencer tick** by three routines that the OS copies into internal SRAM (`$1000000` = OS image
`$2622f4`), called from the tick routine (`$20ad9a`) at `$20b488..$20b49c` after the per-voice loop:

1. **Smoothing** `$10002e0`: for all 16 tracks × 24 parameters (two per 32-bit word),
   `cur = (3·cur + (raw << 7)) >> 2`, masked to `$3fff`. `raw` = kit bytes at `$1000ddc`
   (16 × 24), `cur` = working array at `$1000a4c` (16 × 48 bytes). Same slew as the Monomachine.
   A second smoother (`$100029e`, 48 values at `$1000d7c` from `$1000f5c`) handles global values.
2. **LFO apply** `$1000332`: for each track's LFO (structs at `$1000f8c + $24·k`: destination
   track and parameter, two shape outputs S1/S2 at `+$10/+$14`), `lfo = ((0x3f80 - LFOM)·S1 -
   LFOM·S2) >> 14`, `dest = clamp(cur + (LFOD · lfo) >> 14, 0, $3fff)`, then copies all 16 working
   arrays to the `a6` arrays and restores the un-modulated values. (`$10001e8` = the same for one
   track, used at trigger.)
3. **LFO oscillator** `$1000088` (called when flagged): per LFO, a phase increment from LFOS (param
   21) and a tempo factor at `$100150c` (linear below `$1fff`, cubic above), `phase = (phase + inc)
   & $7fff`, then the OS waveform routine `$204c94(k)`, which calls two of 8 **shape functions**
   through a table at `$2523ee` (args: track, shape 0/1, trigger flag), then the decaying shape
   types 3 (linear fall) and 4 (exponential decay).

**Tick rate:** at 120 BPM the tick routine ran 128.6 times/s = 64 per beat, i.e. the parameter
pipeline is tempo-synced; only the HI08 transport to the DSPs runs at a fixed rate. (To confirm at
other tempos.)

**Cost if we call these routines in a 68k emulator** (measured, `mdProbe count`): smoothing + LFO
0.5-0.65 M 68k instructions/s, LFO shapes ~0.15 M/s, machine functions up to ~0.2 M/s: **~1 M/s in
total**, a few percent of a Force core. For comparison the entire ColdFire OS runs ~4.1-4.8 M/s.

### Host model plan: hybrid

Our C++ owns the tick schedule and the inputs, the MD's routines do the maths:

- C++ writes the kit bytes (`$1000ddc`), LFO settings (struct fields), tempo factor (`$100150c`),
  and trigger flags into a memory image built from section 0 of the user's `.syx` (+ the internal
  SRAM code copy), then calls, in Musashi: smoothing, LFO oscillator, LFO apply, and for each voice
  its machine function with its `a6` array. It writes the 13-word result and trigger code straight
  into DSP2's voice slot.
- Still to translate from the tick routine (`$20ad9a`, the per-voice loop around `$20af52-$20b44c`):
  trigger handling (machine code in word 0, LFO restart per LFO mode), velocity/accent, and the
  DSP1 per-track values it derives from `a6` (volume, pan, sends, effect parameters). This is
  ordinary compiled C, readable with `analysis/dis68.py`.

Alternative, kept as a fallback: run the **whole** ColdFire OS in Musashi (as md-mm does) and drive
it with MIDI. Exact with no translation at all and only ~4-5 M 68k instructions/s, but it brings the
sequencer, the 20 s boot / first-run flash state, the three-processor scheduling md-mm needed, and
MIDI-UART latency and bandwidth limits on every parameter change from the plugin.

## Voice engine prototype: verified bit-exact

`engine/VoiceEngine` runs DSP2's program from the user's `.syx` alone in dsp56300 (Monomodule-style
harness), and `engine/MachineRunner` runs the OS's machine coefficient functions in Musashi.

**DSP2 main loop** (`P:$64-$e7`, once per 32-sample block): for voice k = 0..15, if slot word 0 is
non-zero it calls the machine's init routine on a machine change (`Y:$145af5 + code`) and its
trigger routine (`Y:$145bb6 + code`) and clears word 0; then it calls the machine's render routine
(`Y:$145c77 + code`) into a ping-pong buffer (`Y:$100`/`$120`, pointer in `Y:$140`) and DMAs the 32
samples to the ESSI0 link (voice 0 first waits for the link frame sync on Port C). Init (`$24`):
`$100000`/`$100012` fill unused machine-table entries depending on the memory map (AAR2), `$100024`
sets up ESSI, DMA and interrupts, clears the slots and builds a table at `$148000`.

**Harness:** load section 1's records; set AAR0-3 = `$100539 $140639 $180539 $1c0639` and OMR =
`$00498d` (DSP2's state in md-mm); run from `$24`; ESSI ports get non-blocking silent input and
discarded output; patches: `$73` (per-group handshake word to the ColdFire) → nop; `$b5` → a stub
that sends the voice's 32 samples out over HI08 and continues at `$d5`; `$e2` → wait for a host
"go" word, then `jmp $64`; the silent-voice render's timing-padding loop (`$100093-4`) → nops
(output unchanged).

**Results:**
- Voice 1 (TRX-B2) triggered with md-mm's captured slot words: **6,400/6,400 samples identical** to
  md-mm's link output over 200 blocks.
- `MachineRunner` with md-mm's captured parameter arrays reproduces the slot words **exactly** for
  TRX-B2 (103 68k instructions) and TRX-SD (82 instructions).
- DSP2 cost in the harness: ~4,500 instructions per block for the loop and 16 silent voices
  (6.2 M/s; silent voices could be skipped entirely), ~1,300 more per playing TRX-B2 voice
  (~1.8 M/s per voice).

## Host model: tick scheduling and the per-voice rule (decoded)

- **What drives the tick.** DSP2's main loop sends the ColdFire a word after every 4 voices (group
  0-3, `P:$73`), which raises a ColdFire interrupt (handler `$100043a` in internal SRAM). Group 3
  runs the slot pump (`$10004d0`) and other jobs; group 1 calls the tick routine `$20ad9a`; every
  4th group-2 interrupt flags the LFO oscillator. The tick routine holds a lock and runs with
  interrupts enabled, so the next tick starts only after it finishes: the tick rate is **CPU-bound,
  ~120 Hz** in gearmulator-md-mm (intervals of 12-14 DSP blocks, varying with load) and **not
  tempo-synced**. Smoothing, LFO oscillator and LFO apply run once per tick. On the real machine,
  LFO and smoothing speed therefore vary slightly with load; the host model uses a fixed schedule
  (default one tick per 11 blocks, 125.3 Hz).
- **Tempo** is a factor at `$100150c` = BPM × 24 (default 3000 = 125 BPM), used by the LFO speed and
  by the E12/ROM machines' retrigger times.
- **Per voice, every tick:** the OS calls the voice's machine function on its current `a6` and the
  pump sends the returned words. Word 0 is staged as the trigger flag (1/0) and the pump replaces a
  non-zero value with **current machine id + 1** (UW ids ≥ 128 give bit 7). Idle tracks have the
  empty machine GND-- (`$201128`), whose function returns 2: that is the 2-word `[0, 0]` update.
  MID/CTR machines (ids `$60-$7b`) are not sent. A pending **machine change is applied at the
  trigger**: the track's 24 kit values go straight into the targets, the smoothed array and `a6`
  (no glide), and the voice's function pointer switches.
- **DSP1 per-track effects slot** (`Y:$200+$40·k`, 9 words) is sent by `$1000702` as `a6` words 8-16
  unchanged (AMD..DIST after smoothing and LFO). The 5-word block at `Y:$100+5·k` is computed in the
  tick routine (`$20b1f6-$20b302`): VOL² with velocity/accent scaling, PAN `<<9`, REV² `>>5`,
  DEL² `>>5` (translation pending).
- **Internal SRAM** `$1000000-$1000a2a` is the OS's copy of image `$2622f4`, apart from a few bytes
  of variables near the start; everything after is zero-initialised state.

`engine/HostModel` implements this with the OS's routines in `MachineRunner` (which now holds the
OS's main RAM and internal SRAM). **End to end** (assign machine, set 24 parameters, trigger →
MachineRunner → VoiceEngine): TRX-B2 on track 1 and TRX-SD on track 2 are each **6,400/6,400
samples identical** to gearmulator-md-mm.

## Design consequences

- **All voices from one instance: yes.** One DSP2 renders all 16 voices; the host drives 16 slots.
  Note-number → voice mapping is our own host-model choice.
- **Individual voice outputs: yes**, at the DSP2→DSP1 link — but those are *dry* voices. The MD's
  per-track sound includes its effects page (filter, EQ, SRR, distortion, AMD), which runs on DSP1.
  Per-track outputs *with* those effects need either DSP1's per-track buffers tapped before its
  pan/mix, or the chain reimplemented natively.
- **Machinedrum One needs DSP1's per-track section, not just DSP2.** "Voices only, no master FX" is
  not "DSP2 only": the filter/EQ/distortion that define an MD track are on DSP1.
- **CPU is the main risk on the Force.** Full emulation of both DSPs is ~3-4 Force cores at the
  Monomodule port's efficiency. Options, cheapest first:
  1. DSP2 emulated (machines are complex DSP code), DSP1's per-track chain + mixer **reimplemented
     natively in C++** from its disassembly (standard filters/EQ/SRR/distortion; ~10× cheaper than
     emulation, but "very close" rather than bit-exact), master FX likewise for the FX plugin.
  2. Both emulated, with the static recompiler, plus polyphony limits.
  3. Desktop first (full emulation is fine on a desktop CPU), Force later.

## Open

- The tick routine's per-voice orchestration and DSP1 values (see "Host model plan: hybrid").
- Tick rate vs. tempo (64 per beat at 120 BPM measured).
- The rest of DSP2's 64-word slot (words 13-63: DSP-side voice state?) and where pitch/note enters.
- DSP1's global blocks (`$150-$18c`): master FX parameters and per-tick modulation.
- Whether DSP1 keeps per-track processed blocks in memory before the pan/mix (for per-track outs
  with effects).
- Sample data for E12/ROM machines (DSP2's ~233K external P words).
