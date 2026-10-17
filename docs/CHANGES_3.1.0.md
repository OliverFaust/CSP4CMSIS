# CSP4CMSIS 3.1.0: changes

Minor release. **Opt-in event tracing; no behaviour change, no API change:** without `CSP4CMSIS_TRACE`
the library compiles to the same code as 3.0.1, and code that builds with 3.0.1 builds with 3.1.0 and
behaves the same.

## Change

**Event tracing to CMSIS-View Event Recorder** (`csp4cmsis/inc/csp/csp4cmsis_trace.h`). Define
`CSP4CMSIS_TRACE` for the whole project (channel templates are compiled in application files), add
CMSIS-View:Event Recorder, initialise it (or let the RTOS do it) and enable component 0x50:
`EventRecorderEnable(EventRecordAll, 0x50U, 0x50U)`. Without the define every hook expands to nothing,
its arguments are not evaluated and `EventRecorder.h` is not included.

| msg | Event | Level | Value 1 | Value 2 |
|---|---|---|---|---|
| 0x00 | ProcessStart | Op | context | process object |
| 0x01 | ProcessEnd | Op | context | process object |
| 0x02 | ProcessName | Op | (data: `CSProcess::name()`, after ProcessStart) | |
| 0x10 | RendezvousDone | Op | context of the second partner to arrive | channel |
| 0x20 | AltSelected | Op | context | guard-list index |
| 0x30 | BufferedPut | Op | context | channel |
| 0x31 | BufferedGet | Op | context | channel |
| 0x32 | PutFromISR | Op | context (a put in handler mode) | channel |
| 0x33 | DroppedNew | Op | context | channel (KeepOldest, full: put rejected) |
| 0x34 | OverwroteOldest | Op | context | channel (KeepNewest, full: oldest replaced) |
| 0x40 | SpawnFailed | Error | context | process object |

Context: the thread id in thread mode while the kernel runs, `0x80000000 | IPSR` (the exception number)
in handler mode, 0 in thread mode before the kernel runs. Every put to a buffered channel produces
exactly one record (BufferedPut or PutFromISR, DroppedNew or OverwroteOldest), every get one BufferedGet,
every rendezvous communication exactly one RendezvousDone, recorded inside the critical section in which
the communication is decided. Blocking and unblocking are not traced; the header lists where they would
go.

New files: `trace/CSP4CMSIS.scvd` (event descriptions, also in the pack), `trace/README.md` (use,
invariants, known limitations), `tools/trace/decode.py` (runs `eventlist` with the RTOS's and
CSP4CMSIS's `.scvd` files and writes a CSV: time, component, event, thread, value; channel and process
addresses resolved with `nm`), `tests/trace/drop_policy.cpp` and `check_drop_policy.py` (exact events
of the buffer policies). The pdsc also lists `docs/CHANGES_3.0.1.md`, which the 3.0.1 pack omitted.

Known limitations (`trace/README.md`): Event Recorder's Semihosting variant can write records out of
time-stamp order under preemption (write each record with interrupts masked); RTX5 built with Arm
Compiler 6 needs a main stack of at least 2 KB with Event Recorder; `eventlist` 1.2.0 shows `%t` text of
a data record as empty and mislabels the component column (`decode.py` works around both).

## Verification

Results (2026-10-17; library at `8745b9e` on `trace/event-recorder`, which differs from the release commit
only in documentation):

- **FVP, the 30 configurations of 3.0.1** (Corstone-300 Cortex-M55 and MPS2 Cortex-M4; Arm Compiler
  6.24 and GCC 14.2.1; FreeRTOS 11.3.0 through Arm's adapter, Keil RTX5, FreeRTOS 10.3.1 through ST's
  STM32Cube wrapper; `-O0`/`-O2`/`-Os`; heap-free builds; timer task priorities 2 and 40):
  - **Tracing off:** in all 30 the loaded image is byte-identical to 3.0.1 and the library objects have
    the same size; PASS=36 FAIL=0 SKIP=0 REPLACED=4 (PASS=37 in the 5 heap-free builds).
  - **Tracing on** (Event Recorder 1.6.0, Semihosting variant, DWT time stamps; RTOS thread events and
    errors): the same results in all 30. CSP4CMSIS events decode in all 30 (173,600 to 173,662 per
    run), RTOS thread events in the 26 RTX5 and FreeRTOS 11.3 configurations (ST's FreeRTOS records
    none); logs complete, time stamps monotonic. Content checks, all 30: 500 RendezvousDone on each of
    the two channels of T12, naming the channel; its 1000 AltSelected select guard 0 / 1 exactly when
    the first / second channel just paired; one PutFromISR in handler mode ("irq N") on each buffered
    channel of T2; every ProcessStart precedes the other events of its thread and every ProcessEnd
    follows its ProcessStart; two SpawnFailed in T28; `tests/trace/drop_policy.cpp`: 2 BufferedPut +
    3 DroppedNew and 2 BufferedPut + 3 OverwroteOldest, context "pre-kernel".
  - Two reference runs with the RTOS's default event levels (Corstone-300 GCC RTX5 `-O2`, Arm Compiler 6
    FreeRTOS `-O2`; 0.5 and 5.0 million events): the same results and checks.
  - Cost (Corstone-300, RTX5): library objects +411 B (GCC `-O2`), +466 B (GCC `-O0`), +426 B (Arm
    Compiler 6 `-O2`), +330 B (`-O0`) of code, data/bss unchanged; Event Recorder 1.9–7.0 KB of code and
    1,192 B of RAM (64 records).
  The FVP harness is not public; its logs are not published.
- **Compile:** all library sources and an application file that instantiates every channel type, with
  and without `CSP4CMSIS_TRACE`, at `-O0`/`-O2`/`-Os` (GCC 14.2.1, Cortex-M4): no warnings; without
  `CSP4CMSIS_TRACE` the code and read-only data are identical to 3.0.1.
- **Pack:** built with `scripts/build_pack.py`; `packchk` (CMSIS-Toolbox 2.15): 0 errors, 0 warnings;
  installed with `cpackget` into an empty pack root, a project using `OliverFaust::CSP4CMSIS@3.1.0`
  (ARMCM4, RTX5, GCC) builds without tracing and, with `CSP4CMSIS_TRACE` and CMSIS-View:Event Recorder,
  with tracing.

Not rerun for 3.1.0: the hardware targets (Alif DK-E8, NUCLEO-G474RE), the 3.0 defaults and
dynamic-allocation FVP configurations, and the positive controls (without `CSP4CMSIS_TRACE` the code is
that of 3.0.1). Event Recorder buffer overflow is not testable on the FVP (the Semihosting variant writes
every record to the file at once); it moves to the hardware tests.
