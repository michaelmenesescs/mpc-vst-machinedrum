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

Once a DSP's own header+data record stream is exhausted, the traffic changes character: the
`RUN dspN` count for each DSP in the 5-second capture (352,603 for DSP1, 285,133 for DSP2)
comfortably exceeds each program's word count (18,823 / 250,123), so both DSPs finish loading and
reach real runtime operation inside the capture window. The excess words are genuine runtime
traffic; 36,390 host-command IRQs also occurred in the same window. What that runtime traffic
means — the Machinedrum's equivalent of Monomodule's 52-word parameter block — is not decoded yet.

## What's still open

- The runtime (post-boot) word/IRQ protocol: which words are parameters, how often they're sent,
  and their layout. This is the next tracing step, and the hard part — the Machinedrum equivalent
  of what Monomodule's `HostModel.cpp` reverse-engineered for the Monomachine.
- Whether DSP1 and DSP2 talk to each other only over ESSI (as `mdhardware.h`'s comments say) or
  also exchange anything over their own host ports.
- Sample data: DSP2's program includes ~233K external P words backing the E12/ROM/RAM machines;
  whether that's baked into section 1 itself (looks likely, given the full word-for-word match) or
  loaded separately is not yet checked.
