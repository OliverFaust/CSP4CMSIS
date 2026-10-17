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

Invariants:

- every put to a buffered channel produces exactly one record: BufferedPut
  (PutFromISR in handler mode), DroppedNew (KeepOldest, full) or
  OverwroteOldest (KeepNewest, full; no BufferedPut for it). putFromISR() on a
  full Block channel returns false and is not a put;
- every get produces one BufferedGet;
- every rendezvous communication produces exactly one RendezvousDone,
  recorded by the second of the two partners to arrive.

`tests/trace/drop_policy.cpp` checks the put invariant with exact counts.

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

## Results, FVP regression matrix (Step 1)

30 configurations: Corstone-300 (Cortex-M55) and MPS2 Cortex-M4; Arm Compiler
6.24 and GCC 14.2.1; FreeRTOS 11.3 (CMSIS-FreeRTOS adapter), RTX5 5.9.1 and
ST's FreeRTOS 10.3.1; -O0/-O2/-Os, heap-free and timer-priority variants.
Event Recorder 1.6.0, Semihosting variant, DWT cycle counter time stamps.

- Tracing off: in every configuration the loaded image is byte-identical to
  v3.0.1 and the test results are the same (PASS=36, heap-free PASS=37;
  FAIL=0 SKIP=0 REPLACED=4).
- Tracing on: the same results everywhere. CSP4CMSIS events decode in all 30
  (about 173,600 per run), RTOS thread events in the RTX5 and FreeRTOS 11.3
  configurations (ST's FreeRTOS records none). Logs complete, time stamps
  monotonic.
- Content checks, all 30: 500 RendezvousDone on each of the two channels of
  test T12, each naming the channel object; its 1000 AltSelected select guard
  0 / 1 exactly when the channel that just paired is the first / second; one
  PutFromISR in handler mode on each buffered channel of T2; every ProcessStart
  precedes the other events of its thread, every ProcessEnd follows its
  ProcessStart; two SpawnFailed in T28; five writes, with no reader, into a 2-element
  KeepOldest / KeepNewest channel give exactly 2 BufferedPut + 3 DroppedNew /
  2 BufferedPut + 3 OverwroteOldest.
- Buffer overflow is not testable here: the Semihosting variant writes every
  record to the file as it is recorded. Deferred to hardware.
- Repeated runs of one image: identical without tracing. With tracing, the
  simulator's time-keeping depends on the host at semihosting calls: a few
  time stamps can shift by some hundred cycles, and a timing-sensitive test
  can occasionally take a different path (a few more or fewer thread
  switches). Event counts of the CSP4CMSIS checks above are not affected.

Cost (bytes, text = code + read-only data; Corstone-300, RTX5):

| | GCC -O2 | GCC -O0 | Arm Compiler -O2 | Arm Compiler -O0 |
|---|---|---|---|---|
| library objects, off (= v3.0.1) | 3650 | 5830 | 3775 | 5913 |
| library objects, on | 4061 (+411) | 6296 (+466) | 4201 (+426) | 6243 (+330) |
| Event Recorder object, code | 2394 | 3294 | 6970 | 3896 |
| Event Recorder RAM (64 records) | 1192 | 1192 | 1192 | 1192 |

Library data/bss are unchanged (0/4). Channel templates compiled in
application files grow as well (each traced put/get: one EventRecord2 call).
Arm Compiler -O2 makes the Event Recorder object larger than -O0: it inlines
the record-locking helpers and unrolls the filter loops.
