# CSP4CMSIS 3.2.0: changes

Minor release. **Tracing, a Perfetto view and conformance checking; no behaviour change, no API change:**
without `CSP4CMSIS_TRACE` the library compiles to the same code as 3.1.0 (and 3.0.1), and code that
builds with 3.1.0 builds with 3.2.0 and behaves the same.

## Library API

Unchanged. The book stays on 3.0.1 (the "book API" of 3.0): everything it uses is the same in 3.2.0. The only header text that changed for users is the comment of `IsrChanout::putFromISR()`
(below, "Documented behaviour").

## Tracing (opt-in, `CSP4CMSIS_TRACE`): changes since 3.1.0

**The trace event format changed from 3.1.0 and is experimental.** Event numbers, values and their
order may change in any later release; the 3.x stability promise covers the library API, not the
trace format. A tool written for 3.1.0's records does not read 3.2.0's: use `trace/CSP4CMSIS.scvd` and
`tools/trace/` of the same release.

The full table is in `trace/README.md` and `csp4cmsis/inc/csp/csp4cmsis_trace.h`. Changes:

- **Rendezvous partners.** RendezvousDone (0x10) names the writer thread, the reader thread, the
  channel, and whether the reader recorded it (it arrived second). In 3.1.0: the context of the second
  partner and the channel.
- **Wait and resume events** (Detail level, 0x60–0x6F): WaitChannel (with the direction: write or read),
  WaitBufferedFull, WaitBufferedEmpty, WaitAlt (with the mask of the enabled guards and the number of
  guards), WaitBarrier, WaitJoin, recorded before a call that can block; Resume when it returns,
  ResumeTimeout when an ALT wait ended by its timeout. Together with AltGuard they give each waiting
  process's offers. Filter them by level if not needed: `EventRecorderDisable(EventRecordDetail, 0x50U,
  0x50U)`.
- **Ring-slot indices.** Buffered events carry the ring slot written or read; OverwroteOldest the slot
  evicted and the slot written; DroppedNew `CSP4CMSIS_TRACE_NO_SLOT`.
- **Policy bits.** Buffered events carry the element count after the event, the policy (Block,
  KeepNewest, KeepOldest) and the capacity in one value (count << 16 | policy << 14 | capacity). With
  tracing, a buffered channel's capacity is at most 16383 (`static_assert`).
- **AltGuard** (0x21): each guard of an Alternative, once, before its first select() (input or output
  with the channel, timeout with the delay in ticks, other). **AltDeadline** (0x22): before a WaitAlt
  whose round has an enabled timeout guard, the tick count the wait's time was computed at and the
  deadline tick.
- **Ticks for timeout waits.** After an AltDeadline, Resume and ResumeTimeout also carry the tick count
  after the wait and the deadline tick. No other event records a tick count.
- **The Alternative in AltSelected** (0x20): the Alternative object next to the context and the
  guard-list index, so every select() is attributed from the trace alone.
- **Context values.** RendezvousDone's first two values are now the writer and the reader thread (not
  the context of the second partner); the context encoding is unchanged (thread id; 0x80000000 | IPSR in
  handler mode; 0 before the kernel runs).

No event records any part of a channel's element values. Cost (Corstone-300, RTX5, library objects
with tracing on, compared with tracing off): +1,500 B (GCC `-O2`), +1,466 B (`-O0`), +1,572 B (Arm
Compiler 6 `-O2`), +1,454 B (`-O0`) of code; data and bss unchanged. With tracing off the layout and
code are unchanged.

## Tools (in the repository, not in the pack)

- `tools/trace/decode.py`: **Perfetto export** (`--perfetto`, Chrome Trace Event JSON for
  ui.perfetto.dev): one group per thread with running, waiting and event tracks, flow arrows from
  writer to reader of each rendezvous and from each buffered put to the get that took its value,
  occupancy counters, interrupt tracks; windows by time, test marker or thread.
  `tools/trace/check_perfetto.py` checks such a file with Perfetto's trace_processor.
- `tools/trace/conformance.py` (run by decode.py): ring-slot replay, ALT offers, ALT deadlines and
  **stable-point detection** (every live process waits and the idle thread runs), classified as
  "awaiting environment", "deadlock" or "other" ("stability unconfirmed" without RTOS switch events).
- `tools/trace/csp_trace.py`: **conformance checking against `docs/formal` with ProB**. It projects a
  decoded trace per Alternative, buffered channel and rendezvous channel, matches each projection to a
  model configuration (`tools/trace/mappings/`), and checks with `probcli`:
  - **trace checks**: `MODEL \ H [T= RECORDED`, per select() for ALTs;
  - **stable failures**: at each stable point, the model can be stable after the recorded trace refusing
    what the trace refused;
  - **mutation proofs** (`--mutate`): one fault per configuration family (events swapped, a
    RendezvousDone dropped, a guard index changed, a ResumeTimeout made early, a get given the wrong put,
    an offer removed at a stable point); each must fail;
  - **coverage**: the share of recorded CSP events inside a checked projection.
  - **UNKNOWN handling**: ProB giving up, a time-out, a memory limit or output without a verdict is
    UNKNOWN, never PASS; a mutant is caught only by FAIL; UNKNOWN is not cached.
  - **Memory limit and retry**: each probcli run has a memory limit (`--prob-mem`, default 85 % of the
    memory divided by `--jobs`); a run stopped at the limit is rerun once with 3 x the limit and fewer
    runs at once, and stays UNKNOWN if it fails again.
- `tools/trace/check_fair_select.py`: **fairness** of fairSelect() across selects, from the trace (no
  ProB).

## Models (`docs/formal`)

- **New: `rendezvous_plain.csp`**, the plain operations of a 3.x rendezvous channel (no ALT): pairing,
  the copy and the record of the communication, both arrival orders.
- **Updated to 3.x, all five others:**
  - `alt_one_winner.csp`: the one-winner protocol with re-verification (OWRV) as 3.x implements it. Three
    mismatches with the code were fixed: **A**, a READY state the code does not have; **B**, an ALT
    writer that waited again after a stale wakeup (3.x re-verifies and starts a new round); **C**, no
    late signal from a plain partner (3.x signals after the critical section). `SYSM`: several messages
    per partner, with a bound on the rounds of each select().
  - `alt_owrv_extended.csp`: checked against A–C (already 3.x's protocol). **D**: an activate signals
    the partner's ALT only when it claims the partner at activate, as in 3.x (alt_one_winner too). No
    verdict changed.
  - `alt_timeout_deadline.csp`: a parameterised system (`SYSN`: tick modulus, delays up to 15 ticks, up
    to 3 channel guards).
  - `buffered_channel_v2.csp`: capacity and roles as parameters (`SYS_BUFN`, `SYS5G`, `SYS_KNE`,
    `SYS_KOE`); the putFromISR token window (below) as a documented expected FAIL.
  - `rendezvous_commit_record.csp`: range assertions only.
- **Range assertions** in all six: each value-carrying observed channel carries only values of its
  declared type, within the bounds the conformance checks use (ProB does not type-check channel values).
- **`docs/formal/historical/`**: the 1.x/2.0.0 "trust the wakeup" rendezvous and the 2.0.0 RTOS-timer
  ALT timeouts, moved out of the current models unchanged (`docs/CHANGES_2.0.1.md` cites their
  counterexamples).

Totals (ProB 1.16.1): 213 assertions in the six current models, 175 PASS and 38 FAIL, every verdict as
expected (each FAIL is an intended counterexample, described in its model).

## Documented behaviour

**`putFromISR()` on a Block channel can return false while a slot is free.** A reader returns its
slot's space token only after its get has completed; if it is preempted between the two, the slot is
already free but `putFromISR()` finds no token and returns false. This is accepted behaviour, not a
bug (stated at `IsrChanout::putFromISR()`, `buffered_channel.h` and `BUFFERED_CHANNEL_ANALYSIS.md`;
modelled in `buffered_channel_v2.csp`). **Recommendation:** for interrupt producers that must not lose
samples to this window, use KeepNewest or KeepOldest: their `putFromISR()` decides from the ring count
in the critical section that writes the slot, so it has no such window (they still lose samples when
the buffer is really full).

## Results

Summary (2026-10-26; the FVP harness is not public and its logs are not published):

- **30 FVP configurations** (Corstone-300 Cortex-M55 and MPS2 Cortex-M4; Arm Compiler 6.24 and GCC
  14.2.1; FreeRTOS 11.3.0 through Arm's adapter, Keil RTX5 5.9.1, FreeRTOS 10.3.1 through ST's
  STM32Cube wrapper; `-O0`/`-O2`/`-Os`, heap-free and timer-priority variants).
- **Tracing off:** in all 30 the loaded image is byte-identical to v3.1.0; PASS=36 FAIL=0 SKIP=0
  REPLACED=4 (PASS=37 in the heap-free builds).
- **Tracing on** (Event Recorder 1.6.0, Semihosting variant): the same test results in all 30; every
  trace test and content check passes (`trace/README.md`, "Results").
- **Conformance** (all 30 and the demo network): 1,122,784 trace checks and 640 stable-failure checks,
  all PASS; **coverage 91.8 %** of the recorded CSP events in a checked projection; **mutants: 820 of
  910 caught** (per run 29 of 32 on Corstone-300, 28 of 31 on MPS2 Cortex-M4, 19 of 22 with ST's
  FreeRTOS); the 90 others are the 3 known uncaught swaps in each run (below). The demo's 6 of 6.
  Fairness: 0 violations.
- **Models:** 213 assertions, 175 PASS / 38 FAIL as expected.
- **Host tests:** the decoder, conformance and csp_trace tests (56), and the ALT selection test
  (5.9 million cases).

## Known limitations

- **Liveness only in fixed model configurations.** That a select() over channel guards returns once a
  partner offers is proved by `[FD=` only in fixed configurations (two rendezvous inputs; crossing
  ALTs; a rendezvous and an interrupt-fed buffered guard; a Block channel of 2 with an ALT end). The
  parameterised configurations used by the conformance checks have safety and deadlock-freedom
  assertions only. No model proves liveness for three channel guards without a timeout, or for a
  buffered guard with several task partners.
- **Model bounds.** Timeout guards are matched up to 15 ticks (at most 3 channel guards), buffered
  channels up to capacity 4; longer delays and larger capacities are reported as unmatched, with the
  reason, and not checked.
- **No switch events on ST's FreeRTOS wrapper.** It records no RTOS thread events: no "running" track
  in Perfetto, and its stable points are "stability unconfirmed", so no stable-failure checks there.
- **Three uncaught swaps** (per run; the model allows both orders, so the mutant is a valid trace):
  - alt7: AltSelected and the writer's Resume: the code allows both orders (alt_one_winner);
  - buf14: the moved Resume drops out of the projection, and puts do not name their writer (Part 5G);
  - buf5: the model's stale flag may come before any signal, the code's cannot (Part 5).
- From 3.1.0: the Semihosting variant can write records out of time-stamp order under preemption (write
  each record with interrupts masked); RTX5 with Arm Compiler 6 needs a main stack of at least 2 KB
  with Event Recorder; eventlist 1.2.0 shows a data record's `%t` text as empty (decode.py works
  around it). Event Recorder buffer overflow is not testable on the FVP.

## Requirements (tracing and tools only)

| Tool | Version used | For |
|---|---|---|
| CMSIS-View pack (Event Recorder) | 1.2.0 (Event Recorder 1.6.0) | recording |
| CMSIS-Toolbox: cbuild, `eventlist` | cbuild 2.14, eventlist 1.2.0 | building; decode.py |
| RTOS `.scvd` files | CMSIS-RTX 5.9.1 (`RTX5.scvd`), CMSIS-FreeRTOS 11.3.0 (`FreeRTOS.scvd`) | decoding RTOS events |
| GNU `nm` (`arm-none-eabi-nm`) | from Arm GNU Toolchain 14.2.1 | channel and process names |
| Python | 3.11 and 3.13 (standard library only) | decode.py, csp_trace.py, checkers |
| Python package `perfetto` | 0.58.2 | check_perfetto.py only |
| ProB (`probcli`) | 1.16.1 | csp_trace.py `--prove` |

The pack itself needs nothing new: without `CSP4CMSIS_TRACE` the requirements are those of 3.1.0.

## Pack

The pdsc lists `docs/CHANGES_3.2.0.md`; `trace/CSP4CMSIS.scvd`, `trace/README.md` and the trace header
were already in the 3.1.0 pack. Tools, models and tests stay in the repository, as before.
