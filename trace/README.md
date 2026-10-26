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

## Using the tools in your own project

The library side is in the pack; the tools are in this repository
(`tools/trace/`, Python 3, standard library only). Nothing below depends
on the FVP harness used for the results at the end, which is not public.
The trace format is experimental: use the `.scvd` and the tools of the same
release as the library (`docs/CHANGES_3.2.0.md`).

**1. Enable tracing and Event Recorder.**

- Define `CSP4CMSIS_TRACE` for the whole project (the channel templates are
  compiled in your files), e.g. in the `.csolution.yml` or `.cproject.yml`:

      define:
        - CSP4CMSIS_TRACE

- Add Event Recorder from the CMSIS-View pack (tested: 1.2.0):
  - on an FVP or a simulator, the Semihosting variant, which writes every
    record to a file on the host: `- component: ARM::CMSIS-View:Event
    Recorder&Semihosting`; run the FVP with semihosting enabled
    (`-C cpu0.semihosting-enable=1` on Corstone-300,
    `-C armcortexm4ct.semihosting-enable=1` on MPS2 Cortex-M4). The file is
    `EventRecorder.log` in the FVP's working directory, or the path you
    define as `EVENT_LOG_FILENAME` (a string literal, with its quotes);
  - on hardware with a debug probe, the DAP variant: `- component:
    ARM::CMSIS-View:Event Recorder&DAP`. The records stay in a RAM buffer
    that the debugger reads (Event Recorder window of µVision or Keil
    Studio; give it `trace/CSP4CMSIS.scvd` to name the events). decode.py
    reads the Semihosting variant's log file; it was not tested with logs
    saved by a debugger.
- Initialise Event Recorder: RTX5 does it in osKernelInitialize() when
  `OS_EVR_INIT` is 1 (RTX_Config.h; its default is 0), CMSIS-FreeRTOS before
  main() (`configEVR_INITIALIZE`, default 1); otherwise call
  `EventRecorderInitialize(EventRecordError, 1U)` yourself. Then enable
  CSP4CMSIS's component, after osKernelInitialize():

      EventRecorderEnable(EventRecordAll, 0x50U, 0x50U);

  Time stamps: the source is the DWT cycle counter by default
  (`EVENT_TIMESTAMP_SOURCE` 0 in EventRecorderConf.h); set
  `EVENT_TIMESTAMP_FREQ` to its frequency so that decode.py prints seconds.
  On an FVP the counter runs on model cycles: decode with `--model-time`.
- RTOS thread events (switches, creation) let the Perfetto view show when
  each thread runs and let decode.py confirm stable points. RTX5 records
  them with its default event settings (`OS_EVR_THREAD`), CMSIS-FreeRTOS
  through `freertos_evr.h`, which its FreeRTOSConfig.h template includes.
  ST's CMSIS-RTOS2 wrapper records none.
- Size: a main stack of at least 2 KB for RTX5 with Arm Compiler 6 (Known
  limitations), and room for the buffer (64 records: 1,192 bytes of RAM).

**2. Decode** the log to CSV, a summary and the stable points:

    tools/trace/decode.py -a app.elf -I <RTOS>.scvd EventRecorder.log \
        -o events.csv --summary summary.txt --stable stable.csv [--model-time]

`<RTOS>.scvd`: `RTX5.scvd` of the CMSIS-RTX pack, or
`CMSIS/RTOS2/FreeRTOS/FreeRTOS.scvd` of the CMSIS-FreeRTOS pack, in your
pack root (omit `-I` without RTOS events). Needs `eventlist` (CMSIS-Toolbox;
tested 1.2.0) and `arm-none-eabi-nm` on the PATH, or `EVENTLIST=` / `NM=`.
The summary ends with one line per library-state check (ring slots, ALT
offers, ALT deadlines, stable points); the CSV has one row per event.

**3. View it in Perfetto:**

    tools/trace/decode.py -a app.elf -I <RTOS>.scvd EventRecorder.log --perfetto trace.json \
        [--model-time] [--window T0:T1] [--threads A,B]

Open https://ui.perfetto.dev, "Open trace file", and pick trace.json. The
file is read in the browser and not uploaded. Each thread is a group
(running, waiting, events); interrupts and buffered channels have groups of
their own (section Perfetto below). Flow arrows (writer to reader of a
rendezvous; a buffered put to the get that took its value) are drawn for
selected slices: click a slice, or **drag-select an area** of the timeline
to see the arrows of everything in it (also listed in the "Flow events" tab
below the timeline). **Never use Perfetto's Share button for a private
trace**: it uploads the trace to Google's servers and makes a link that
anyone who has it can open. Select a window or
threads for long logs (a whole run of 650,000 events gives 80 MB).

**4. Check conformance** against the models in `docs/formal`:

    tools/trace/csp_trace.py events.csv --stable stable.csv --out conf --mutate
    tools/trace/csp_trace.py --prove conf --cache probcache [--jobs N]
    tools/trace/check_fair_select.py events.csv --fair <regex of your fairSelect() Alternatives>

The first command projects the trace and matches the projections against
the model configurations (`conf/match.txt`: what matched, and why the rest
did not); the second runs ProB 1.16.1 (`probcli` on the PATH or
`PROBCLI=`) and writes the verdicts into `conf/report.json`. A projection
is checked only if it lies within the models' bounds (timeouts up to 15
ticks, capacities up to 4; section Conformance below). Read the result:

- every trace and stable-failure check PASS (`report.json`: `verdicts`);
  any FAIL is a recorded behaviour the model does not allow (the check's
  file is in `conf/checks/`); UNKNOWN means ProB did not decide (time,
  memory) and is not a pass;
- with `--mutate`, every mutant FAIL (caught), except the three swaps of
  Known limitations if your trace has those situations;
- the coverage in `report.json` (`coverage`): the share of your CSP events
  that lie in a checked projection;
- check_fair_select.py: 0 violations.

The `--prove` step needs memory: `--jobs` ProB runs at once, each limited to
85 % of the machine's memory divided by `--jobs`; a run stopped at the
limit is retried once with 3 x the limit.

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
  application's own start function. The tests run one after the other and
  build their processes in one static pool (`trace_pool.h`, 3.5 KB) instead
  of static objects each: the Corstone-300 trace builds have little RAM to
  spare.
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
    interrupt from the platform, see the file);
  - `stable_multi.cpp`: three processes -- an ALT with a timeout guard, a
    one-guard ALT on an interrupt-fed Block channel, a plain rendezvous
    reader -- waiting together: stable points awaiting a timeout and an
    interrupt, released by the timeout and by the interrupt in either order
    (`check_stable_multi.py`; the same periodic interrupt);
  - `fair_select.cpp`: two plain writers with offers pending before every
    fairSelect() of their reader (it sleeps a tick, then marks the select's
    start with a put and get on its own buffered channel), so both guards
    are ready throughout every select (`tools/trace/check_fair_select.py
    --evidence fair_a`);
  - `throttled_drop.cpp`: an interrupt puts into a KeepOldest channel of 2
    elements faster than the network drains it: a sampler takes each sample
    with a one-guard ALT and forwards it on a plain rendezvous channel to a
    consumer that sleeps a tick after each read. The sampler waits to write
    (backpressure), the channel fills and the interrupt's puts are dropped
    during operation, not only after the sampler's last select. The counts
    depend on when the interrupts fall, so `check_throttled_drop.py` checks
    invariants: a DroppedNew before the sampler's last select, every
    DroppedNew at count == capacity, puts = gets + dropped + still buffered,
    the occupancy never above the capacity, a DroppedNew while the sampler
    waits to write. It uses the periodic interrupt of stable_env.cpp at a
    higher rate: the platform also provides
    `csp4cmsis_trace_env_timer_rate(hz)`, the rate of the next start (in
    the timer's clock; stopping restores the default).
- `check_alt_deadlines.py`: the ALT deadlines of the suite's timeout tests
  T20-T24 and of alt_offers.cpp's two Alternatives with a timeout guard
  (time left, deadline per select(), ResumeTimeout at or after the
  deadline, Resume before or at it, T23's zero timeouts never wait).
- `tests/host/trace_decoder/test_conformance.py` (host, Python):
  tools/trace/conformance.py on synthetic event streams, including the
  "deadlock" classification (no target test deadlocks).
- `tests/host/trace_decoder/test_csp_trace.py` (host, Python, no ProB):
  tools/trace/csp_trace.py and check_fair_select.py on synthetic rows: tick
  placement, channel guards and takes, per-select attribution, matching
  reasons, the plain-rendezvous mapping, put identities, the policy field,
  ALT ends per thread, the stable-failure environment (an interrupt-fed
  channel's puts are awaited, not refused), chunked traces, the mutations
  and the fairness rule.

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

    tools/trace/csp_trace.py events.csv --stable stable.csv --out DIR [--mutate] [--models DIR]
    tools/trace/csp_trace.py --prove DIR [DIR ...] [--jobs N] [--cache DIR] [--prob-mem GB]

The first form builds projections of a decoded trace, matches them against
the models of `docs/formal` (or `--models`), and writes DIR/report.json
(projections, matching, checks, coverage) and DIR/match.txt; the second
checks the assertions of one or more such directories with `probcli` of
ProB 1.16.1 (PATH or `PROBCLI=`) and writes the verdicts back. Identical
assertions are checked once; the trace checks of one model configuration
form one assertion (the recorded traces as one external choice, so the
model is normalised once), split when it fails.

- **Projections.** One Alternative object (an AltGuard with index 0 starts
  a new one, also at a reused address), one buffered channel, one
  rendezvous channel, with the events of the threads that use it. AltGuard
  and AltSelected name the Alternative and the thread, buffered events the
  policy: nothing is inferred.
- **Mappings.** `tools/trace/mappings/<model>.json`, from each model's
  header (Trace conformance, `SigmaObs`, `H`): the events a trace supplies
  and their model events, the configurations and what an instance needs to
  match them, the stable-failure rules and the gaps. Buffered elements are
  put identities: the write index mod K, K = capacity + 2 of the model
  configuration; a get carries the index of the write that last wrote the
  slot it read.
- **Instance matching.** Every projection is matched against each model
  configuration; the others are listed with the reason. The models'
  parameters (docs/formal: SYSN, SYSM, SYS_BUFN, SYS5G, SYS_KNE, SYS_KOE)
  take capacity, roles, tick modulus, timeouts and channel guards from the
  instance.
- **Model bounds.** An instance outside the bounds the parameterised models
  were checked for is not matched; the report lists it with the reason.
  - Timeout guards up to 15 ticks, at most 3 channel guards
    (alt_timeout_deadline's SYSN): "timeout N ticks (the model's delays
    are 0..15)".
  - Buffered capacities up to 4 (SYS_BUFN, SYS5G, SYS_KNE, SYS_KOE):
    "capacity N (the parameterised models are checked up to capacity 4)".

  Why: ProB does not type-check channel values, so a value outside a
  model's checked range is not rejected; the recorded trace would be
  checked against a model never verified for it. The models' range
  assertions (each value-carrying observed channel carries only values of
  its declared type) are checked up to these bounds. Capacity 6 did not
  finish in ProB.
- **Per-select cut.** An Alternative with several select()s is checked one
  select() at a time (alt_timeout_deadline; two plain rendezvous inputs also
  alt_one_winner's SYSM with one select). Each select() is checked from the
  model's initial state: its window opens after the Alternative's previous
  AltSelected and closes with its AltSelected. Scope: this checks each
  select() on its own; it does not check properties across selects (order,
  fairness, delivery of every message), and it inherits the model's partner
  abstraction: each channel guard's partner offers at most once per
  select(). The report counts the selects outside it (a partner with more
  than one put, get or plain operation on the guard's channel inside the
  window).
- **Fairness across selects** (`tools/trace/check_fair_select.py`, no
  ProB), on the uncut sequence of selects. fairSelect(): select() enables
  the guards in the order (i + offset) % n and stops at the first ready one,
  re-verifies in that order after a wakeup, a claimed guard wins, else the
  first ready one; offset starts at 0 and becomes (selected + 1) % n after
  each select(); priSelect() always uses 0. Checked consequence: a guard
  ready during the whole select() that precedes the selected guard in that
  order is a violation. Ready throughout: a plain writer's offer recorded
  (pair WaitChannel) before the select's window opens and taken after it
  closes, or a buffered channel with elements and no get in the window;
  the window opens at the ALT thread's last record before the select.
- **Trace check.** `MODEL \ H [T= RECORDED` for every matched projection or
  select.
- **Stable failures.** At a stable point t (decode.py `--stable`) of a
  checked projection, X = its observable events except the environment
  events the point awaits (`tick` when an enabled timeout guard waits;
  `isr_put` when a read waits on an interrupt-fed channel). `MODEL \ H` is
  composed with an environment that performs t, then a marker `mark_t`,
  then offers only X; before the marker it can always take an escape event
  to a loop, so the composition cannot deadlock before the marker. It
  deadlocks iff the model can be stable after t refusing X:
  `:[deadlock free [F]]` must fail with a counterexample ending in
  `mark_t`. Ticks of a select without timeout guards may occur freely
  during t and are blocked after the marker, not refused; the timeout
  model's channel partner is the one that never offers when t has no take;
  the plain rendezvous model's sides may stop after any operation (lazy).
  A buffered projection is checked only where every user of the channel
  waits on it. Points with stability unconfirmed are not checked.
- **Mutations** (`--mutate`): per model configuration family, one fault on
  a copy of the decoded rows of a checked projection or select: two of its
  events swapped, a RendezvousDone dropped, an AltSelected guard index
  changed, a ResumeTimeout turned into a Resume one tick before its
  deadline, a BufferedGet given the next put identity (after the slot
  replay); and at every stable point one offer removed (the first whose
  removal changes a refusal set). Each must fail.
- **Verdicts.** Every ProB run gives PASS, FAIL or UNKNOWN. UNKNOWN covers
  ProB giving up (a node timed out, "may not be"), the run hitting its time
  limit or its memory limit, or output without a verdict. UNKNOWN is never
  PASS, a mutant is caught only by FAIL, and UNKNOWN results are not cached.
- **Memory limit.** Each probcli runs under `--prob-mem` GB (default
  `PROB_MEM_GB`, else 85 % of the machine's memory divided by `--jobs`; 0:
  none): in a cgroup of its own (`systemd-run --user --scope`, no swap),
  where it is killed at the limit, or else with `RLIMIT_AS`, where an
  allocation fails. Either way the run is UNKNOWN with the reason in the
  report, never an empty or missing result. Such a run (a batch is not
  split) is rerun once, after all others, with 3 x its limit (at most 85 %
  of the memory) and as many runs at once as fit in 85 % of the memory. One
  still UNKNOWN then stays UNKNOWN; its checks keep both attempts
  (`prob.attempts` in `report.json`), and `report.json`'s `prob` counts the
  runs, the retries and those still UNKNOWN.
- **Coverage.** The share of the recorded CSP events (RTOS events
  excluded) inside a checked projection or select, per kind.

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
- Liveness of a select() over channel guards (it returns once a partner has
  offered) is proved only in fixed configurations, by `[FD=`:
  - two rendezvous input guards: an ALT writer and a plain writer, two ALT
    writers, a plain writer and a signal (alt_one_winner, OWRV);
  - crossing ALTs with an output and an input guard each, and an ALT writer
    with two output guards (alt_owrv_extended Parts A and C);
  - a rendezvous guard and an interrupt-fed buffered guard
    (alt_owrv_extended Part B);
  - a Block channel of capacity 2 with an ALT reader or an ALT writer
    (buffered_channel_v2 Part 5), and an ALT reader fed by an interrupt
    (Part 1c).

  The parameterised configurations used by the conformance checks have no
  liveness assertion:
  - alt_timeout_deadline's SYSN with channel guards only (safety, `OFFS`,
    and deadlock freedom);
  - alt_one_winner's SYSM with several messages per partner (deadlock
    freedom and the round bound);
  - buffered_channel_v2's SYS5G (trace refinement and deadlock freedom).

  No model proves liveness for three channel guards without a timeout, or
  for a buffered guard with several task partners.

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
- Tracing on: the same results everywhere. 361,900 to 362,400 CSP4CMSIS
  events per run (per run: 6,102 to 6,104 RendezvousDone, 7,198 to 7,200
  WaitChannel, ~25,300 WaitBufferedFull, ~40,410 WaitBufferedEmpty, 21,045
  to 21,219 WaitAlt, 36,356 to 36,370 AltSelected, 123 AltGuard,
  46 AltDeadline, ~55,540 BufferedPut and BufferedGet, ~20,160
  OverwroteOldest, 3 DroppedNew, 7 PutFromISR, 40 WaitBarrier, 7 WaitJoin,
  30 ResumeTimeout); RTOS thread events in the RTX5 and
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
  (read), all "awaiting environment"; tests/trace/stable_multi.cpp gives
  2 PutFromISR and 2 stable points with the offers of all three of its
  processes, its 4 points all "awaiting environment". RTX5 and FreeRTOS
  11.3: 8 stable points per run, all "awaiting environment" (the suite's
  tests do not run CSP processes); ST's FreeRTOS (no switch events): 74,
  all "stability unconfirmed". No "deadlock" anywhere; that classification is tested on
  the host (tests/host/trace_decoder).
- ALT deadlines, all 30 (decoder and tests/trace/check_alt_deadlines.py):
  46 waits with a timeout guard, each after its AltDeadline, time left
  1..the shortest timeout, one deadline per select(); 30 ResumeTimeout, all
  at the deadline tick (0 ticks late); 16 Resume, all before the deadline.
  T20: 5 selects each for timeouts 1, 3 and 10, time left 1, 3 and 10, all
  by ResumeTimeout; T21: 15 selects, 6 the channel, 9 the timeout; T22: the
  5-tick guard of three; T23: both zero-timeout Alternatives select without
  waiting; T24: 10 waits (9 stale wakeups) with one deadline, then
  ResumeTimeout.
- Throttled drop, all 30 (tests/trace/check_throttled_drop.py; the totals
  above and below are without this test): the sampler's 20 selects, 20 gets,
  maximum occupancy 2, 2 samples still buffered at the end. The interrupt
  rate depends on the platform's timer clock: Corstone-300, 30 to 31 puts,
  8 or 9 dropped, 6 or 7 of them during operation, 6 or 7 while the sampler
  waits to write (38 waits); MPS2 Cortex-M4, 97 to 100 puts, 75 to 78
  dropped, 66 to 69 during operation, 64 to 73 while the sampler waits to
  write (39 waits). Conformance, checked separately against the models with
  their parameters: the sampler's Alternative (20 selects),
  throttled_samples (Part 2bN, SYS_KOE(2, 0, -1)) and throttled_forward
  (plain rendezvous) match; all trace and stable-failure checks pass (2 per
  projection on Corstone-300, 1 on MPS2, none on ST's FreeRTOS) and the
  mutants fail (a swap, a dropped RendezvousDone, a changed get identity,
  the removed offers).
- Conformance (tools/trace/csp_trace.py, ProB 1.16.1, the committed models
  of docs/formal with their parameters; 2026-10-26), all 30 and the demo
  (two runs):
  per run 68 of 74 Alternatives match (36,390 select()s, each checked on its
  own: T1, T12, T13, T15 and the rest; 10,087 of them, the ALT writers of T1b
  and T13b and one select of T2, are outside the partner abstraction), 22 of
  27 buffered channels and the 5 plain rendezvous channels (these totals
  include throttled_drop.cpp); the demo's merger (62/63 selects),
  sensor_samples and merger_to_consumer. Not matched: timeouts above 15
  ticks (T11, T15s, T22), two selects that record only AltGuard, and
  channels with two ALT ends or a multi-guard ALT end (T4a, T4b, T10, T15,
  T15s). All 1,122,784 trace checks pass (37,416 to 37,430 per run) and all
  640 stable-failure checks (25 per run on Corstone-300, 24 on MPS2
  Cortex-M4; none with ST's FreeRTOS, which has no switch events). Mutants:
  per run 22 trace mutants (10 swaps, 3 dropped RendezvousDone, 2 changed
  guards, 6 changed get identities, 1 early Resume) and, with switch events,
  one removed offer per stable point (10 on Corstone-300, 9 on MPS2): all
  fail as they must except 3 swaps, which swap events the model allows in
  either order (alt_one_winner: AltSelected and the writer's Resume; Part
  5G: a blocking writer's Resume and another writer's BufferedPut; Part 5: a
  PutFromISR and a stale Resume). In all 820 of 910 mutants caught; the 90
  others are these 3 swaps in each run. The demo's 6 mutants fail. Coverage:
  91.8 % of the CSP4CMSIS events per run lie in a checked projection or
  select (0.04 % before the per-select cut and the parameterised models):
  RendezvousDone 99.8 %, AltSelected 100 %, buffered 96.2 %, waits and
  resumes 87.0 %; the largest unchecked groups are T15's channels. Fairness
  (check_fair_select.py): fair_select.cpp 16 fairSelect()s, 15 with both
  guards ready throughout, 0 violations, all 30; T12 0 violations (no select
  with another guard ready throughout), the demo's merger 0 (1 such select).
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
| library objects, on | 5150 (+1500) | 7296 (+1466) | 5347 (+1572) | 7367 (+1454) |
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
ResumeTimeout) add 156 / 136 / 176 / 176 bytes. AltSelected and AltGuard
naming the Alternative and the thread, and the buffered events' policy field,
add 60 / 24 / -162 / 24 bytes (Arm Compiler -O2 inlines differently); the
policy is a template constant, folded with the capacity into one constant
at compile time, and AltSelected becomes a 4-word record (32 bytes in the buffer instead of
16). With tracing off the layout and code are unchanged.

Library data/bss are unchanged (0/4). Channel templates compiled in
application files grow as well (each traced put/get: two or three
EventRecord calls). Arm Compiler -O2 makes the Event Recorder object larger
than -O0: it inlines the record-locking helpers and unrolls the filter loops.
