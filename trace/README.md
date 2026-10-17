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
RendezvousDone names the writer and the reader thread; buffered-channel
events carry the number of elements after the operation and the capacity.

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

`tests/trace/drop_policy.cpp` checks the put invariant with exact counts.

## Perfetto

    tools/trace/decode.py -a app.elf -I <RTOS>.scvd EventRecorder.log --model-time --perfetto trace.json

writes Chrome Trace Event JSON; open it at https://ui.perfetto.dev (Open trace
file; the trace stays in the browser). Each thread is a group named after the
process (`CSProcess::name()`), else the symbol of its control block (static
allocation; e.g. the RTOS's idle thread), else "thread 0x...", with:

- **running**: when the thread runs, from the RTOS's switch events (RTX5
  ThreadSwitched; FreeRTOS TaskSwitchedIn/Out). Without switch events
  (e.g. ST's FreeRTOS wrapper) there are none, and the JSON's otherData says
  so;
- **waiting**: "waiting on <kind> <object>" from a Wait event to its Resume
  (args: object, timeout); a thread still blocked when the log ends gets
  "(no resume)";
- **events**: CSP4CMSIS events as instants with their values (channel names
  resolved, guard index, element count), and a short "rendezvous <channel>"
  slice on the writer and on the reader, joined by a flow arrow (writer to
  reader).

Group "buffered channels": one counter per channel, elements = puts minus
gets (drops and overwrites change nothing). Group "interrupts": one track
per exception number with the events recorded in handler mode.

Time: microseconds of the log's time stamps (count / EVENT_TIMESTAMP_FREQ).
On a simulator with `--model-time` these are model cycles, not real or
simulated time (otherData "time").

Selection: `--window T0:T1` (seconds; either side may be empty), `--threads
name,name`. Test windows: if the application records a marker (a data record
with the test's name, component COMP, message MSG) at the end of each test,
`--marker COMP:MSG --test NAME` selects from the previous marker to NAME's.

`tools/trace/check_perfetto.py trace.json` (Python package `perfetto`) loads
a trace with Perfetto's trace_processor and checks it with SQL: one flow per
RendezvousDone, one running slice per switch-in, every wait slice ended by
its Resume, occupancy within 0..capacity.

Size: a whole run of the FVP regression suite (about 650,000 events) gives an
80 MB JSON file, which ui.perfetto.dev loads in seconds; select a window or
threads for longer logs.

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

- Tracing off: in every configuration the loaded image is byte-identical to
  v3.1.0 (and 3.0.1) and the test results are the same (PASS=36, heap-free
  PASS=37; FAIL=0 SKIP=0 REPLACED=4).
- Tracing on: the same results everywhere. About 361,500 CSP4CMSIS events
  per run, of which about 188,000 are wait/resume events (per run: 6,060 to
  6,062 RendezvousDone, ~7,090 WaitChannel, ~25,280 WaitBufferedFull,
  ~40,390 WaitBufferedEmpty, ~21,100 WaitAlt, 40 WaitBarrier, 2 WaitJoin,
  26 ResumeTimeout); RTOS thread events in the RTX5 and FreeRTOS 11.3
  configurations (ST's FreeRTOS records none). Logs complete, time stamps
  monotonic.
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
  (context "pre-kernel").
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
| library objects, on | 4790 (+1140) | 6899 (+1069) | 4947 (+1172) | 6945 (+1032) |
| Event Recorder object, code | 2394 | 3294 | 6970 | 3896 |
| Event Recorder RAM (64 records) | 1192 | 1192 | 1192 | 1192 |

Library data/bss are unchanged (0/4). Channel templates compiled in
application files grow as well (each traced put/get: two or three
EventRecord calls). Arm Compiler -O2 makes the Event Recorder object larger
than -O0: it inlines the record-locking helpers and unrolls the filter loops.
