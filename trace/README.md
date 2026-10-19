# Event Recorder tracing

Opt-in: build the whole project with `CSP4CMSIS_TRACE` defined, add
CMSIS-View:Event Recorder, initialise it (or let the RTOS do it) and enable
component 0x50:

    EventRecorderEnable(EventRecordAll, 0x50U, 0x50U);

Without `CSP4CMSIS_TRACE` nothing changes and CMSIS-View is not needed. The
events are listed in `csp4cmsis/inc/csp/csp4cmsis_trace.h`; `CSP4CMSIS.scvd`
decodes them. Decode a log (Semihosting variant) to CSV with

    tools/trace/decode.py -a app.elf -I <RTOS>.scvd EventRecorder.log -o events.csv --summary summary.txt

(`eventlist` from the CMSIS-Toolbox and `arm-none-eabi-nm` on the PATH, or
`EVENTLIST=` / `NM=`). On a simulator pass `--model-time`: time stamps then
count model cycles, not simulated seconds.

## What is recorded

Every event names its context: the thread (thread mode, kernel running),
handler mode as 0x80000000 | IPSR (decode.py: "irq N", N the exception
number) or 0 in thread mode before the kernel runs ("pre-kernel"). Channel
events name the channel object, process events the process object.
RendezvousDone names the writer and the reader thread. No event records any
part of a channel's element values: only library state.

| msg | event | values (a, b, c, d) |
|---|---|---|
| 0x00 | ProcessStart | context, process |
| 0x01 | ProcessEnd | context, process |
| 0x02 | ProcessName | data: the process's name() |
| 0x10 | RendezvousDone | writer thread, reader thread, channel, 1 if the reader recorded it |
| 0x20 | AltSelected | context, Alternative, guard-list index, 0 |
| 0x21 | AltGuard | context, Alternative, target, guard-list index << 8 \| kind (0 input, 1 output: target the channel; 2 timeout: target the delay in ticks; 3 other) |
| 0x22 | AltDeadline | context, Alternative, tick count the wait's time was computed at, deadline tick (only before a WaitAlt with an enabled timeout guard) |
| 0x30 | BufferedPut | context, channel, elements after << 16 \| policy << 14 \| capacity (policy: 0 Block, 1 KeepNewest, 2 KeepOldest), ring slot written |
| 0x31 | BufferedGet | as BufferedPut, ring slot read |
| 0x32 | PutFromISR | as BufferedPut (handler mode) |
| 0x33 | DroppedNew | as BufferedPut, 0xFFFFFFFF (no slot) |
| 0x34 | OverwroteOldest | as BufferedPut, slot evicted << 16 \| slot written |
| 0x40 | SpawnFailed | context, process |
| 0x60 | WaitChannel | context, channel \| direction (bit 0: 1 write, 0 read) |
| 0x61 | WaitBufferedFull | context, channel |
| 0x62 | WaitBufferedEmpty | context, channel |
| 0x63 | WaitAlt | context, Alternative, enabled guards (bit i: guard-list index i), number of guards |
| 0x64 | WaitBarrier | context, barrier |
| 0x65 | WaitJoin | context, completion semaphore |
| 0x6E | Resume | context, the wait's object (as the wait recorded it); after an AltDeadline also the tick count after the wait and the deadline tick |
| 0x6F | ResumeTimeout | context, Alternative, tick count after the wait, deadline tick (an ALT wait ended by its timeout) |

Buffered channels: a capacity of at most 16383 when tracing (static_assert:
14 bits next to the 2-bit policy). AltSelected and AltGuard name the
Alternative object and the thread, so every select() is attributed from the
trace itself (no RTOS switch events needed). Cost: AltSelected is a 4-value
record (32 bytes in the recorder's buffer instead of 16, +8 bytes in a
Semihosting log, per select()); AltGuard and the buffered events keep their
size.
OverwroteOldest replaces the oldest element in place, so the slot evicted
and the slot written are the same slot (both are recorded). AltGuard is
recorded once per guard, by the thread that calls the Alternative's first
priSelect()/fairSelect() (again from guard 0 for a new Alternative at the
same address, and for guards added later); the API has no guard
preconditions, so a select() that waits has every guard enabled. WaitChannel
also records the side that waits for an ALT writer to be copied from
(output guard, direction write).

Ticks (osKernelGetTickCount(), modulo 2^32) are recorded only for ALT waits
with a deadline: a round with an enabled timeout guard records AltDeadline
(the tick count the remaining time was computed at, and the deadline tick:
that count plus the timeout passed to the RTOS) just before its WaitAlt,
and its Resume or ResumeTimeout carries the tick count after the wait and
the deadline. Every other wait is recorded as before. Cost per wait with a
deadline: AltDeadline (a 4-value record: 32 bytes in the recorder's
buffer, 32 bytes in a Semihosting log) and a 4-value instead of a 2-value
Resume (+16 bytes buffer, +8 bytes log), so +48 bytes in the buffer, +40
bytes in the log, and one more osKernelGetTickCount() call.

Wait events (Detail level, 0x60-0x65) are recorded before a call that can
block: WaitChannel (rendezvous: the end is busy, or waiting for the
partner), WaitBufferedFull, WaitBufferedEmpty, WaitAlt, WaitBarrier,
WaitJoin (Run with TerminatingNetwork). Resume follows when the call
returns, ResumeTimeout when an ALT wait ended by its timeout; the thread
records nothing in between. A wait that did not block gives a short slice.
Filter them out by level if they are not needed:
`EventRecorderDisable(EventRecordDetail, 0x50U, 0x50U)`.

Invariants:

- every put to a buffered channel produces exactly one record: BufferedPut
  (PutFromISR in handler mode), DroppedNew (KeepOldest, full) or
  OverwroteOldest (KeepNewest, full; no BufferedPut for it). putFromISR() on a
  full Block channel returns false and is not a put;
- every get produces one BufferedGet;
- every rendezvous communication produces exactly one RendezvousDone,
  recorded by the second of the two partners to arrive.

Trace tests (tests/trace; trace builds only, each with a checker that reads
decode.py's output):

- `drop_policy.cpp` (before the kernel starts): the put invariant with
  exact counts, and ring slots: BufferedPut slots 0, 1, then DroppedNew x3 /
  OverwroteOldest evicting slots 0, 1, 0 (`check_drop_policy.py`,
  `check_ring_slots.py`);
- `trace_tests.cpp` starts a thread that runs the others, then the
  application's own start function:
  - `rendezvous_order.cpp`: the rendezvous invariant for plain ends (a
    Chanout and a Chanin, no ALT) in both arrival orders: 10 communications
    in which the writer arrives first (RendezvousDone recorded by the
    reader, by_reader=1) and 10 in which the reader arrives first
    (by_reader=0) (`check_rendezvous_order.py`);
  - `ring_wrap.cpp`: puts and gets interleaved on a Block channel of 3 so
    that the ring wraps twice, with the exact slot of each
    (`check_ring_slots.py`);
  - `alt_offers.cpp`: three Alternatives with a timeout guard, a rendezvous
    output guard and a buffered (full) output guard; exact AltGuard events,
    one WaitAlt each with mask 0x3 of 2 guards, the selected guard
    (`check_alt_offers.py`, which also checks every WaitAlt of the suite's
    test T12: mask 0x3 of 2);
  - `stable_env.cpp`: two processes that wait for an interrupt-fed buffered
    channel between interrupts: stable points "awaiting environment", then
    the interrupt releases them (`check_stable_env.py`; needs a periodic
    interrupt from the platform, see the file).
- `check_alt_deadlines.py`: the ALT deadlines of the suite's timeout tests
  T20-T24 and of alt_offers.cpp's two Alternatives with a timeout guard
  (time left, deadline per select(), ResumeTimeout at or after the
  deadline, Resume before or at it, T23's zero timeouts never wait).
- `tests/host/trace_decoder/test_conformance.py` (host, Python):
  tools/trace/conformance.py on synthetic event streams, including the
  "deadlock" classification (no target test deadlocks).
- `tests/host/trace_decoder/test_csp_trace.py` (host, Python, no ProB):
  tools/trace/csp_trace.py on synthetic rows: tick placement, matching
  reasons, put identities, policy evidence, AltGuard thread inference, the
  stable-failure environment and the mutations.

## Perfetto

    tools/trace/decode.py -a app.elf -I <RTOS>.scvd EventRecorder.log --model-time --perfetto trace.json

writes Chrome Trace Event JSON; open it at https://ui.perfetto.dev (Open trace
file; the trace stays in the browser). Each thread is a group named after the
process (`CSProcess::name()`) with a summary of its roles, taken from the
trace (e.g. "Merger — ALT in: a_to_merger, b_to_merger, sensor_samples; out:
merger_to_consumer": channels it read through an ALT, read directly, wrote).
Threads that are not CSP4CMSIS processes (no ProcessName) are named after the
symbol of their control block (static allocation; e.g. the RTOS's idle
thread), else "thread 0x...". Each group has:

- **running**: when the thread runs, from the RTOS's switch events (RTX5
  ThreadSwitched; FreeRTOS TaskSwitchedIn/Out). Without switch events
  (e.g. ST's FreeRTOS wrapper) there are none, and the JSON's otherData says
  so;
- **waiting**: "waiting on <kind> <object>" from a Wait event to its Resume
  (args: object, timeout; an ALT wait with a deadline also wait_tick,
  deadline_tick, resume_tick); a thread still blocked when the log ends
  gets "(no resume)";
- **events**: CSP4CMSIS events as instants with their values (channel names
  resolved, guard index), a short "rendezvous <channel>" slice on the writer
  and on the reader, joined by a flow arrow (writer to reader), and the
  thread's buffered puts and gets (below).

Buffered channels: the decoder replays each channel's buffer as a FIFO in
record order (every put is one record, every get takes the oldest element).
A put is a short slice "put <channel>" on the track of its context (the
thread, the "irq N" track in handler mode, "pre-kernel"); a get is a slice
"get <channel> ← <source>", joined by a flow arrow from the put whose value
it took. The arrows have category "buffered", the rendezvous ones
"rendezvous". Puts whose value is never read are named by their fate:
"(overwritten, never read)" (KeepNewest, replaced by a later put),
"(dropped)" (KeepOldest, buffer full), "(still buffered at the end)". Args:
channel, context, outcome or source, the raw event and its values, the
replayed depth and the library's element count after the event (equal, or
the log is incomplete). A window that opens with elements in a buffer shows
their gets with the source but without an arrow (`source_before_window`).

Group "buffered channels": one occupancy counter per channel. Group
"interrupts": one track per exception number with the events recorded in
handler mode. Group "stable points": one slice per stable point (below),
named by its classification, with the offers and the reason as args.

## Library state: ring slots, ALT offers, stable points

decode.py feeds every CSP4CMSIS event of the log (whatever the window) to
tools/trace/conformance.py:

- ring slots: each buffered channel is replayed as a FIFO; every get must
  read the slot written by the put it takes, that slot must not have been
  written again in between, every OverwroteOldest must evict the slot of the
  oldest put, DroppedNew uses no slot, and the replayed depth must equal the
  library's count;
- ALT offers: every enabled bit of a WaitAlt must resolve through the
  Alternative's AltGuard events to (channel, direction) or a timeout;
- ALT deadlines: a WaitAlt with an enabled timeout guard follows an
  AltDeadline (and no other WaitAlt does); the time left (deadline - tick)
  is at least 1 and at most the shortest enabled timeout; all waits of one
  select() have the same deadline (it is fixed when select() starts); every
  ResumeTimeout is at or after the deadline tick; every Resume of such a
  wait before it or at it (a flag in the tick before the deadline, resumed
  in it: counted apart as "at"); no WaitAlt has a zero-tick timeout guard
  enabled (it is ready at once, so that select() never waits);
- stable points: a moment when every live CSP process (ProcessStart without
  ProcessEnd) is in an open wait and the RTOS's idle thread runs (its
  control block's symbol contains "idle", or `--idle NAME`). Its offers are
  the open waits' offers (channel read/write, timeouts, barrier, join). It
  is classified when it begins: "awaiting environment" (a wait that only a
  timeout or an interrupt-fed buffered channel can end: one that PutFromISR
  writes somewhere in the log), "deadlock" (no two offers match and nothing
  awaits a timeout or an interrupt) or "other" (matching offers while every
  process waits, or waits that are not channel offers, with the reason).
  Without RTOS switch events (ST's FreeRTOS) the same moments are reported
  as "stability unconfirmed".

`--summary` ends with one line per check, `--checks FILE` writes the
results as JSON, `--stable FILE` the stable points (CSV if FILE ends in
.csv, else JSON).

Time: microseconds of the log's time stamps (count / EVENT_TIMESTAMP_FREQ).
On a simulator with `--model-time` these are model cycles, not real or
simulated time (otherData "time").

Selection: `--window T0:T1` (seconds; either side may be empty), `--threads
name,name`. Test windows: if the application records a marker (a data record
with the test's name, component COMP, message MSG) at the end of each test,
`--marker COMP:MSG --test NAME` selects from the previous marker to NAME's.

`tests/trace/demo_network.cpp` is a small network with named processes
(ProducerA, ProducerB, Merger with a fair ALT and a timeout, Consumer) and an
interrupt-fed KeepNewest channel, for trying the view: the application runs
`csp4cmsis_trace_demo_network()` from a thread and calls
`csp4cmsis_trace_demo_sensor_isr()` from a periodic interrupt.

`tools/trace/check_perfetto.py trace.json` (Python package `perfetto`) loads
a trace with Perfetto's trace_processor and checks it with SQL: one flow per
RendezvousDone, one running slice per switch-in, every wait slice ended by
its Resume, occupancy within 0..capacity; buffered channels: one flow per get
and no get without a source put, puts (+ elements buffered at the window
start) = gets + overwritten + dropped + still buffered at the end, and the
replayed depth equal to the library's element count at every put and get;
library state: no ring-slot, ALT-offer or ALT-deadline error and (whole log) one stable
slice per stable point.
These counts come from the same log, so they cannot show an event the
library never recorded. `--expect expected.json` adds the counts known from
the application (flows per rendezvous channel; puts, gets, overwritten,
dropped and still buffered per buffered channel) and fails on any difference
or on a channel not listed.

Size: a whole run of the FVP regression suite (about 650,000 events) gives an
80 MB JSON file, which ui.perfetto.dev loads in seconds; select a window or
threads for longer logs.

## Conformance against the formal models (csp_trace.py)

    tools/trace/csp_trace.py events.csv --stable stable.csv --out DIR --run [--mutate] [--cache DIR]

(`probcli` of ProB 1.16.1 on the PATH or `PROBCLI=`.) From a decoded trace
it builds projections, matches them against the models of `docs/formal`,
writes one CSPm file per check (it includes the model) and, with `--run`,
checks every distinct assertion once (`--cache`: also across runs).

- **Projections.** One Alternative object (an AltGuard with index 0 starts
  a new one, also at a reused address), one buffered channel, or one
  rendezvous channel, with the events of the threads that use it. AltGuard
  has no context value: its thread is the running thread from the RTOS
  switch events; without them (ST's FreeRTOS) it is inferred from the next
  record naming the Alternative or the next AltSelected of a thread that is
  not blocked.
- **Mappings.** `tools/trace/mappings/<model>.json`, from each model's
  header (Trace conformance, `SigmaObs`, `H`): the events a trace supplies
  and their model events, the configurations and what an instance needs to
  match them, the stable-failure rules and the gaps. Buffered elements are
  put identities: the write index mod K, K = capacity + 2 of the model
  configuration; a get carries the index of the write that last wrote the
  slot it read. Ticks: alt_timeout_deadline.json.
- **Instance matching.** The models are fixed configurations (one or two
  select()s, capacities 1 and 2, given partners). Every projection is
  matched against each configuration; the others are listed with the
  reason (e.g. "timeout 10 ticks (the model's delays are 0..3)", "5040
  select()s (the model is one select())", "capacity 4 (Part 3: 1)").
  Projections are checked whole; none is cut.
- **Trace check.** `MODEL \ H [T= RECORDED` for every matched projection
  (`a_sel.g.e.b` renamed to `asel.g`: e and b are not recorded).
- **Stable failures.** At a stable point t (decode.py `--stable`) of a
  matched projection, X = its observable events except the environment
  events the point awaits (`tick` when an enabled timeout guard waits;
  `isr_put` when an interrupt-fed read waits). `MODEL \ H` is composed with
  an environment that performs t, then a marker `mark_t`, then offers only
  X; before the marker it can always take an escape event to a loop, so the
  composition cannot deadlock before the marker. It deadlocks iff the model
  can be stable after t refusing X: `:[deadlock free [F]]` must fail with a
  counterexample ending in `mark_t`. Environment events a projection does
  not observe (ticks of a select without timeout guards) may occur freely
  during t and are blocked after the marker, not refused. The model's
  channel partner is then the one that never offers (PARTNER(0)) when t has
  no transfer: PARTNER(1)'s pending offer is a hidden event, so that
  configuration is never stable. Points with stability unconfirmed (no
  switch events) are not checked.
- **Mutations** (`--mutate`): per model, one fault on a copy of the decoded
  rows of a matched projection: two of its events swapped, a
  RendezvousDone dropped, an AltSelected guard index changed, a
  ResumeTimeout turned into a Resume one tick before its deadline, a
  BufferedGet given the next put identity (after the slot replay: a
  capacity-1 channel has one slot), and the timeout offer removed at a
  stable point. Each must fail.
- **Gaps.** AltSelected does not name its Alternative: a select() that
  neither waits nor is the object's first is attributed to the thread's
  current Alternative. The buffered policy is known only from evidence
  (WaitBufferedFull: Block; OverwroteOldest: KeepNewest; DroppedNew:
  KeepOldest). The rendezvous models (alt_one_winner, alt_owrv_extended)
  are matched only: their activate events need a replay of the state words
  (model headers, Gaps), and no recorded instance matches them yet.

## Known limitations

- Event Recorder's Semihosting variant reads the time stamp and then writes
  the record. If an interrupt or a task switch comes in between, records with
  later time stamps reach the log first, so the log is not always in
  time-stamp order. Fix: write each record with interrupts masked, e.g. a
  wrapper around EventRecord2/EventRecord4/EventRecordData that saves PRIMASK,
  disables interrupts, records and restores PRIMASK (GNU ld --wrap or Arm
  Compiler $Sub$$/$Super$$).
- RTX5 built with Arm Compiler 6 initialises the kernel (and Event Recorder)
  before main(), from the C library start-up; its events are recorded on the
  main stack (SVC handler). With Event Recorder this needs a main stack of at
  least 2 KB; a 512-byte main stack overflows.
- eventlist 1.2.0 prints %t text of a data record (ProcessName) as empty and
  labels every event with the last component it read from the .scvd files.
  decode.py takes the text from the log and the component from the event ID.

## Results, FVP regression matrix

30 configurations: Corstone-300 (Cortex-M55) and MPS2 Cortex-M4; Arm Compiler
6.24 and GCC 14.2.1; FreeRTOS 11.3 (CMSIS-FreeRTOS adapter), RTX5 5.9.1 and
ST's FreeRTOS 10.3.1; -O0/-O2/-Os, heap-free and timer-priority variants.
Event Recorder 1.6.0, Semihosting variant, DWT cycle counter time stamps.

- The trace tests run before the suite (tests/trace: drop_policy.cpp before
  the kernel starts, the others in trace_tests.cpp's thread, which then
  starts the suite's runner). On Corstone-300 the trace builds use a smaller
  RTOS heap, to make room for the tests' threads: FreeRTOS 4 KB instead of
  16 KB, RTX5 8 KB instead of 16 KB (the suite uses less than 1.4 KB).
- Tracing off: in every configuration the loaded image is byte-identical to
  v3.1.0 (and 3.0.1) and the test results are the same (PASS=36, heap-free
  PASS=37; FAIL=0 SKIP=0 REPLACED=4).
- Tracing on: the same results everywhere. 361,700 to 362,100 CSP4CMSIS
  events per run (per run: 6,084 to 6,086 RendezvousDone, 7,160 to 7,162
  WaitChannel, ~25,290 WaitBufferedFull, ~40,390 WaitBufferedEmpty, 21,070
  to 21,570 WaitAlt, 115 AltGuard, 44 AltDeadline, ~55,530 BufferedPut and
  BufferedGet,
  ~20,160 OverwroteOldest, 3 DroppedNew, 5 PutFromISR, 40 WaitBarrier,
  5 WaitJoin, 28 ResumeTimeout); RTOS thread events in the RTX5 and
  FreeRTOS 11.3 configurations (ST's FreeRTOS records none). Logs complete,
  time stamps monotonic.
- Content checks, all 30: 500 RendezvousDone on each of the two channels of
  test T12, naming the channel, with the test's two sender threads as
  writers and its runner as reader; writer and reader differ in every
  RendezvousDone; its 1000 AltSelected select guard 0 / 1 exactly when the
  first / second channel just paired; one PutFromISR in handler mode
  ("irq N") on each buffered channel of T2; every ProcessStart precedes the
  other events of its thread, every ProcessEnd follows its ProcessStart;
  every wait is followed by a Resume of the same thread and object before
  that thread's next CSP4CMSIS event (one wait per run is still open when
  the suite ends); the 26 ResumeTimeout fall in the timeout-guard tests;
  two SpawnFailed in T28; tests/trace/drop_policy.cpp gives exactly
  2 BufferedPut + 3 DroppedNew and 2 BufferedPut + 3 OverwroteOldest
  (context "pre-kernel"); tests/trace/rendezvous_order.cpp gives exactly
  10 RendezvousDone with by_reader=1 (writer first) and 10 with by_reader=0
  (reader first), each naming the pair's writer and reader.
- Library state, all 30: no ring-slot error (every get read its put's slot,
  every OverwroteOldest evicted the oldest put's slot, every DroppedNew
  without slot); tests/trace/ring_wrap.cpp and drop_policy.cpp give exactly
  the expected slot sequences; every enabled bit of every WaitAlt resolves
  (none unresolved), tests/trace/alt_offers.cpp's three Alternatives give
  exactly their guard lists, masks 0x3 of 2 and selections, and every
  WaitAlt of test T12 has mask 0x3 of 2; tests/trace/stable_env.cpp gives
  3 PutFromISR and stable points offering env_samples (read) and env_link
  (read), all "awaiting environment". RTX5 and FreeRTOS 11.3: 4 stable
  points per run, all "awaiting environment" (the suite's tests do not run
  CSP processes); ST's FreeRTOS (no switch events): 25, all "stability
  unconfirmed". No "deadlock" anywhere; that classification is tested on
  the host (tests/host/trace_decoder).
- ALT deadlines, all 30 (decoder and tests/trace/check_alt_deadlines.py):
  44 waits with a timeout guard, each after its AltDeadline, time left
  1..the shortest timeout, one deadline per select(); 28 ResumeTimeout, all
  at the deadline tick (0 ticks late); 16 Resume, all before the deadline.
  T20: 5 selects each for timeouts 1, 3 and 10, time left 1, 3 and 10, all
  by ResumeTimeout; T21: 15 selects, 6 the channel, 9 the timeout; T22: the
  5-tick guard of three; T23: both zero-timeout Alternatives select without
  waiting; T24: 10 waits (9 stale wakeups) with one deadline, then
  ResumeTimeout.
- Conformance (tools/trace/csp_trace.py, ProB 1.16.1), all 30: 31
  projections per run match a model configuration (29 select()s of
  alt_timeout_deadline: T20 with timeouts 1 and 3, T23, the single-guard
  ALTs of T2, T4, T7, T9, T10 and alt_offers.cpp's alt_timeout and alt_full;
  Part 3 of buffered_channel_v2 for alt_full_buf; Part 1c for T9's channel);
  all 930 trace checks pass (6 events at most, up to 9,041 ProB states,
  20 distinct assertions, 50 s of ProB CPU in all). The stable point of
  alt_full (RTX5 and FreeRTOS 11.3: 26 runs) passes its stable-failure
  check. All 266 mutants fail as they must. Not matched (reasons in the
  report): selects with timeouts above 3 ticks, Alternatives with several
  select()s or channel guards (T1, T12, T13, T15), channels of other
  capacities or roles, and every rendezvous-model configuration.
- Perfetto: test T12 (Corstone-300, RTX5 GCC -O2 and FreeRTOS AC6 -O2; MPS2
  Cortex-M4, ST's FreeRTOS GCC -O2) and a whole run (RTX5 GCC -O2: 80 MB)
  pass check_perfetto.py and load in ui.perfetto.dev in seconds.
- Buffer overflow is not testable here: the Semihosting variant writes every
  record to the file as it is recorded. Deferred to hardware.
- Repeated runs of one image are not always identical: the simulator's
  time-keeping can depend on the host. Without tracing this was seen rarely
  (MPS2 Cortex-M4 model, under host load); with tracing (semihosting calls)
  more often: a few time stamps shift by some hundred cycles, and a
  timing-sensitive test can take a different path (a few more or fewer
  thread switches). Builds are reproducible, and the results and checks
  above are not affected.

Cost (bytes, text = code + read-only data; Corstone-300, RTX5):

| | GCC -O2 | GCC -O0 | Arm Compiler -O2 | Arm Compiler -O0 |
|---|---|---|---|---|
| library objects, off (= v3.1.0) | 3650 | 5830 | 3775 | 5913 |
| library objects, on | 5090 (+1440) | 7272 (+1442) | 5509 (+1734) | 7343 (+1430) |
| Event Recorder object, code | 2394 | 3294 | 6970 | 3896 |
| Event Recorder RAM (64 records) | 1192 | 1192 | 1192 | 1192 |

Of these, the ring slots, channel directions and ALT offers (WaitAlt with
the guard mask, AltGuard) add 144 / 237 / 386 / 222 bytes. Their guard list
costs, with tracing on only: one virtual function per guard class
(traceDescribe, 10 to 12 bytes each at GCC -O2) and its vtable slot (4
bytes per class), Alternative::traceGuards() (68 bytes at GCC -O2), 4 bytes
of RAM per Alternative (the number of guards described), and one AltGuard
record per guard, recorded once per Alternative object (32 bytes in the
recorder's buffer). WaitAlt is a 4-word record (32 bytes in the buffer
instead of 16). The deadline ticks (AltDeadline, 4-value Resume and
ResumeTimeout) add 156 / 136 / 176 / 176 bytes. With tracing off the layout
and code are unchanged.

Library data/bss are unchanged (0/4). Channel templates compiled in
application files grow as well (each traced put/get: two or three
EventRecord calls). Arm Compiler -O2 makes the Event Recorder object larger
than -O0: it inlines the record-locking helpers and unrolls the filter loops.
