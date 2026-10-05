# BufferedChannel / ALT regression suite (Corstone-300 FVP)

`bc_tests.cpp` is the regression suite behind `BUFFERED_CHANNEL_ANALYSIS.md` and the 2.0 BufferedChannel
work (`docs/CHANGES_2.0.md`). It is **test code**: it deliberately uses `csp::internal` classes, backend
heap queries and armlink symbol patching, and is not part of the pack.

The same source builds against:
- **either library generation:** v1.0.0 (`BufferedChannel<T, P>(capacity)`) or v2
  (`BufferedChannel<T, SIZE, P>`, detected via `CSP4CMSIS_BUFFERED_CHANNEL_API`);
- **either CMSIS-RTOS2 backend:** FreeRTOS 11.3.0 through `ARM::CMSIS-FreeRTOS`, or Keil RTX5 5.9.1
  through `ARM::CMSIS-RTX`;
- **three targets:** the Corstone-300 FVP (Cortex-M55, this README), the MPS2 Cortex-M4 FVP (Armv7E-M,
  `results/mps2_m4/`, below) and the Alif DK-E8 board (`docs/hardware_results_dk_e8.md`). Target
  differences are the `BC_*` macros at the top of `bc_tests.cpp` (software-interrupt source and
  priority, sweep ranges); their defaults are the Corstone-300 values.

Each test prints `RESULT <id>: PASS | FAIL | SKIP -- <property>`, where FAIL means the defect is present.
A `SUMMARY` line follows, and then EOT, which ends the FVP run.

## Current results (`results/`)

| Library | Configurations | Result |
|---|---|---|
| **2.1.0**, `release-2.1.0` @ `c66a8b8` (`results/v2.1.0/`) | **30**: Corstone-300 AC6 6.24 and GCC 14.2.1 × FreeRTOS/RTX5 × `-O0`/`-O2`/`-Os`, plus NoHeap × AC6/GCC (16); MPS2 M4 AC6/GCC × FreeRTOS/RTX5 × `-O0`/`-O2` (8), ST wrapper GCC (`FreeRTOS-ST`, `-O2`, `-NoHeap`, `-TP2`) and `FreeRTOS-TP2`/`-TP40` (6) | **PASS=36 FAIL=0 SKIP=0 REPLACED=4 in all 30** (PASS=37 with T19 in the 4 NoHeap builds); compile checks (`compile_checks.txt`): all 24 probes pass with AC6 and GCC on FreeRTOS, RTX5 and ST's wrapper |
| 2.0.1 with the 2.1.0 suite (positive control, `results/v2.1.0/controls_2.0.1/`) | Corstone-300 FreeRTOS GCC; MPS2 M4 FreeRTOS-ST GCC | PASS=32 FAIL=3 SKIP=1: T28, T29, T30 fail as they must; T31 SKIP (no `SleepFor(Time)`) |
| 2.0, `buffered-channel-v2`: OWRV rendezvous/signal channels (`08c6d8a`), C1/C2 API (`0cca916`), static Barrier (`7c176c7`), migrated harness | **12**: Arm Compiler 6.24 and GCC 14.2.1 × `-O0`/`-O2`/`-Os` × FreeRTOS/RTX5 | **PASS=24 FAIL=0 SKIP=0 REPLACED=4 in all 12.** T13/T13b: 0 spins in every sweep; T15: 0 bad trials |
| 2.0, heap-free builds (see "Heap-free proof") | **4**: `FreeRTOS-NoHeap`, `RTX5-NoHeap` × AC6/GCC, `-O0` | **PASS=25 FAIL=0 SKIP=0 REPLACED=4** (T19 included); RTOS heap used: 0 B |
| v1.0.0 @ `a789d2a` (regression baseline; 26-test suite of `11858f6`) | AC6 `-O0`, FreeRTOS and RTX5 | PASS=8 FAIL=17 SKIP=1 on both |
| **MPS2 Cortex-M4 FVP** (Armv7E-M), 2.0 @ `73f46b7` (library sources as `c60665d`) | **8**: AC6 and GCC × `-O0`/`-O2` × FreeRTOS/RTX5 | **PASS=24 FAIL=0 SKIP=0 REPLACED=4 in all 8**; every sweep BUG=0/ANOMALY=0, both regimes; T13/T13b 0 spins; T15 0 bad trials |
| MPS2 Cortex-M4 FVP, v1.0.0 @ `a789d2a` (current suite) | AC6 `-O0` and GCC `-O2`, FreeRTOS and RTX5 | PASS=8 FAIL=18 SKIP=2 in all 4 (the same 18 failures) |

- **REPLACED** = the defect cannot be written any more: the API that allowed it was removed, and a compile
  check in `tests/compile_checks/` proves the removal. Not counted as PASS.

  | Test | Compile check(s) |
  |---|---|
  | T15i, T16a | `neg_rendezvous_isr.cpp`, `neg_rendezvous_isr_writer.cpp` |
  | T16s | `neg_signal_isr.cpp`, `neg_signal_putfromisr.cpp` |
  | T16n | `neg_rendezvous_policy.cpp` (also `neg_signal_policy.cpp`) |

- **T15, T15s** (the late wakeup and the lost signal) run and pass on the OWRV protocol.
- The source still builds against v1.0.0: 1.x code paths are selected by the absence of
  `CSP4CMSIS_ALT_PROTOCOL_OWRV` / `CSP4CMSIS_ISR_WRITER_API`.
- **Harness RTOS heap:** 16 KB (FreeRTOS `configTOTAL_HEAP_SIZE`, RTX5 `OS_DYNAMIC_MEM_SIZE`; FVP test
  branch `f364bf2`). The suite uses at most 1.4 KB, and none for CSP4CMSIS objects.

Positive control for the other toolchain and optimisation levels: v1.0.0 built with GCC `-O2` and with
AC6 `-O2` (FreeRTOS) gives the same PASS=6 FAIL=12 SKIP=1 as at `-O0` (19-test suite, before T14). So T2's
`--wrap` detector and the race sweeps stay sensitive there.

History:
- `d39835b` failed T13/T13b (livelock); this is the only positive control for T13/T13b, since v1.0.0
  never had that livelock. (`6920d1c` is the same commit, identical tree, before the history rewrite of
  2026-09-27 that removed the co-author trailers; it is on no branch.)
- `a85e17d` fixed it with a one-tick back-off;
- `cb10b12` replaced the back-off with semaphore-count readiness.

## Heap-free proof (`FreeRTOS-NoHeap`, `RTX5-NoHeap` build types)

Two extra build types of the harness disable RTOS dynamic allocation completely. The
whole suite runs on them with Arm Compiler 6 and GCC.

| Build type | RTOS configuration | What proves "no dynamic RTOS allocation" |
|---|---|---|
| `FreeRTOS-NoHeap` | `configSUPPORT_DYNAMIC_ALLOCATION=0`: FreeRTOS's dynamic-allocation API is not compiled; **no heap implementation is linked** (the Heap component is left out) | `pvPortMalloc`/`vPortFree` are defined only as counting traps (`noheap_stubs.c`), because the CMSIS-FreeRTOS adapter references them without checking the setting. **AC6:** the linker removes the traps, so nothing in the image references an allocator at all. **GCC:** the traps are linked and T19 reports 0 calls |
| `RTX5-NoHeap` | `OS_DYNAMIC_MEM_SIZE=0`: **no dynamic memory pool** (`osRtxInfo.mem.common == NULL`, `os_mem` absent from the image). Any object created without static memory would get NULL, which CSP4CMSIS treats as fatal | T19 checks that the pool is absent; the suite passing shows every object was created statically |

- **Common evidence:** T17 (50 constructions of `Channel` + `SignalChannel` + `Barrier`, no allocation);
  "heap used = 0 B" in the SUMMARY line; the CSP4CMSIS object files reference no `malloc`, `operator new`,
  `pvPortMalloc` or `osRtxMemoryAlloc`. (Sized `operator delete` is referenced by the deleting
  destructors of classes with virtual destructors, and is never called.)
- **Workarounds needed only because of the RTOS packages** (harness project only; upstream issues, ready to
  file, not filed: `docs/upstream/CMSIS-FreeRTOS_clib_os_dynamic_mutex.md`,
  `docs/upstream/CMSIS-FreeRTOS_pvPortMalloc_unconditional.md`):
  - CMSIS-FreeRTOS 11.3.0 `clib_os.c` (Arm C library locks, AC6) falls back to the dynamic
    `xSemaphoreCreateMutex()` without checking `configSUPPORT_DYNAMIC_ALLOCATION`. A forced include
    (`noheap_shim.h`) makes that fallback yield NULL, so the adapter's static pool of 5 mutexes is used.
  - CMSIS-FreeRTOS 11.3.0 `cmsis_os2.c` references `pvPortMalloc()`/`vPortFree()` unconditionally
    (`osThreadEnumerate()`, `osMemoryPool*()`), hence the traps.
  - RTX5 with Arm Compiler: the C library's stream mutexes (`rtx_lib.c` `_mutex_initialize()`) are
    created with `osMutexNew(NULL)`. Without dynamic memory they come from RTX5's fixed, statically
    allocated mutex pool (`OS_MUTEX_OBJ_MEM=1`, `OS_MUTEX_NUM=8`). Without that pool the image hangs
    before `main()`. CSP4CMSIS itself creates no mutex.
- **The C library's own heap** (`malloc`, used by stdio) is outside the RTOS and still linked. CSP4CMSIS
  never calls it.

## Harness

The FVP results come from a private Corstone-300 / MPS2 Cortex-M4 FVP harness, which is not public. It is
a CMSIS-Toolbox (csolution) project that builds `bc_tests.cpp` and this repository's library, loaded as a
local pack from the checkout's `.pdsc` (an installed CSP4CMSIS pack is not used), and runs the image on
the FVPs. Its settings, as far as they affect the results:

- **Targets:** Corstone-300 FVP (`FVP_Corstone_SSE-300_Ethos-U55`, Cortex-M55) and MPS2 Cortex-M4 FVP
  (`FVP_MPS2_Cortex-M4`, device `ARM::ARMCM4`, Cortex_DFP 1.2.0, 25 MHz core clock,
  `BC_SWI_IRQn = Interrupt0_IRQn`), both Fast Models 11.28.32; stdout on the CMSDK UART0; the run
  ends at the EOT after the `SUMMARY` line. CMSIS-Toolbox 2.14.1.
- **Backends:** Arm's CMSIS-RTOS2 adapter with FreeRTOS 11.3.0 (`ARM::CMSIS-FreeRTOS`), Keil RTX5 5.9.1
  (`ARM::CMSIS-RTX`), and on the M4 also ST's CMSIS-RTOS2 wrapper with FreeRTOS 10.3.1 from STM32CubeG4
  1.6.3.
- **Build types:** each backend at `-O0` (compiler default), `-O2` and (Corstone-300) `-Os`; heap-free
  build types (see "Heap-free proof"); on the M4 also timer task priorities 2 and 40. Toolchains: Arm
  Compiler 6.24 and GCC 14.2.1 (ST's wrapper: GCC only).
- **RTOS settings, all backends:** 100 Hz tick, no time slicing (`OS_ROBIN_ENABLE 0`), timer task
  priority 55 (`osPriorityRealtime7`), 16 KB RTOS heap (FreeRTOS `configTOTAL_HEAP_SIZE`, RTX5
  `OS_DYNAMIC_MEM_SIZE`); RTX5 `OS_STACK_WATERMARK 1`, `OS_THREAD_LIBSPACE_NUM 8`. The Armv7-M FreeRTOS
  port needs the `vPortSVCHandler`/`xPortPendSVHandler` aliases.
- **CSP4CMSIS:** `CSP4CMSIS_STATIC_ALLOCATION`, `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5` (BASEPRI
  0xA0); the backend define per build type.
- **Interposition** (T2, T6, T28): armlink `$Sub$$`/`$Super$$` in `bc_tests.cpp` (Arm Compiler), or
  `-Wl,--wrap=` for `osEventFlagsSet`, `osThreadFlagsSet`, `osSemaphoreRelease`, `osSemaphoreAcquire`,
  `osMessageQueuePut`, `osMessageQueueGet`, `osMessageQueueGetCount`, `osMessageQueueGetSpace`,
  `osMutexAcquire`, `osMutexRelease`, `osTimerNew` and `osThreadNew` (GCC).
- **Timing:** the Corstone-300 FVP tick runs at about 312.5 Hz (the model's core clock is 100 MHz, the
  software assumes 32 MHz). The tests use tick counts only.

## Tests

| Id | Property checked (PASS = holds) |
|---|---|
| T0 | control: a Block BufferedChannel read through ALT delivers 10 values |
| T1a | *sweep*: no lost wakeup when a writer preempts an ALT reader at any point in `select()` |
| T1b | *sweep*: no lost wakeup when a reader preempts an ALT writer at any point in `select()` |
| T2 | no CMSIS-RTOS2 call is made with BASEPRI raised (see "T2 method" below). 2.0 workload also covers the rendezvous and signal paths: plain writer → ALT reader, ALT writer → plain reader, ALT-vs-ALT, signal → ALT reader |
| T3 | *sweep*: KeepNewest keeps every writer's newest value, task vs task |
| T3i | *sweep*: KeepNewest keeps every writer's newest value, task vs ISR `putFromISR()` (I2S_IRQn pended as a software interrupt, priority 6) |
| T4a | two ALT writers: both served, or the second rejected by the assert; never a silent hang |
| T4b | an ALT that never enabled a guard does not cancel another writer's registration |
| T4c | guard state per writer: a second writer's `getGuard()` does not re-target a blocked writer |
| T5 | a large channel (8200 × 4 B, more than the 32 KB heap) is valid after construction, or construction fails loudly |
| T6 | `RelTimeoutGuard` and `Alternative` create no RTOS timer (`osTimerNew()` calls counted, see "T2 method") and use no RTOS heap (200 constructions in a loop). FAIL on 2.0.0 (one timer per guard) |
| T7a | a read via ALT wakes a writer blocked in ALT (control: plain `input()`) |
| T8 | three blocking writers on one Block channel: no loss |
| T9 | a stale ALT wakeup (flag without data) is re-verified, not selected (v2 only; SKIP on v1) |
| T10 | a second ALTing reader is rejected by the assert; the first is served |
| T11 | rendezvous ALT-vs-ALT completes on both ends with the value |
| T12 | rendezvous `fairSelect` over two channels with blocking senders (pipe syntax): 1000 messages, per-channel order |
| T13 | *sweep*: no livelock when a high-priority ALT reader meets a preempted low-priority blocking writer (separate aggressor thread; the runner detects spinning and rescues by lowering the aggressor's priority) |
| T13b | *sweep*: the same for a high-priority ALT writer vs a preempted low-priority blocking reader |
| T14 | a BufferedChannel constructed at namespace scope (before `main()`) works. It also reports the kernel state and static/dynamic `osSemaphoreNew()` results during C++ static initialisation |
| T15i | a rendezvous `putFromISR()` to a waiting ALT reader either delivers the value or returns false. (Today it returns true, and the reader's `select()` returns the channel guard with its destination unchanged.) |
| T15s | `SignalChannel` ALT receiver {X, signal}: a signal that arrives while the receiver takes X (the lower index) is received by the next `select()`, and the sender completes. (Today `unregisterAltIn()` resets the channel, so the signal is lost and the sender blocks for good.) |
| T17 | rendezvous and signal channels and `Barrier` use no RTOS heap: 50 constructions and destructions of `Channel<uint32_t>` + `SignalChannel<>` + `Barrier(3)` allocate nothing (2.0; SKIP on 1.x) |
| T18 | `Barrier(3)` reused for 20 phases by threads of three priorities: nobody leaves a phase before all three arrived (FAIL on 1.0.0: 4 early departures) |
| T20 | timeout accuracy: a timeout of 1, 3, 10 ticks over a never-ready channel is selected after `d` or `d + 1` ticks (5 times each, varied phase) |
| T21 | a timeout (10 ticks) racing a channel whose writer becomes ready 8..12 ticks after `select()` starts: one guard selected; after a timeout the item is still there (plain read); exactly one transfer per trial; 8 ticks: channel, 12 ticks: timeout |
| T22 | several timeout guards (30, 5, 15 ticks): the earliest deadline wins, after 5..6 ticks |
| T23 | zero timeout: selected at once (0 ticks, no wait); a channel that is ready and listed before it wins |
| T24 | a stale ALT wakeup every tick (up to 50) does not postpone a 10-tick timeout. FAIL on 2.0.0 (60 ticks: the timer restarted every round); SKIP on 1.x |
| T25 | `Run(InParallel(…), TerminatingNetwork, prio)` returns after all three processes returned; a process's `taskPriority()` override wins, the others run at `prio`; without `prio`, `osPriorityLow` (2.1.0: `CSP_DEFAULT_NETWORK_PRIORITY`) |
| T26 | `Run(…, StaticNetwork, prio)` returns before any (lower-priority) process ran; threads named after `name()`; all three run afterwards |
| T27 | `forEachProcess` visits the three processes in order; `stackHighWaterMarkWords()` is `CSP_STACK_HWM_UNAVAILABLE` before `Run()`, afterwards between 1 and (stack − 16) words (each process touches 16 words) |
| T28 | a failed `osThreadNew()` (forced through the interposition, see "T2 method") in `Run()` is a fatal error and `Run()` does not return, in both modes. FAIL on 2.0.1 (prints and continues) |
| T29 | a 17th guard in an `Alternative` is a fatal error. FAIL on 2.0.1 (ignored) |
| T30 | `Seconds()`/`Milliseconds()`: 0 stays 0; otherwise never shorter than requested and at most one tick longer, computed in 64 bits (`ms * freq` above 2³²); saturated at `0xFFFFFFFE`. FAIL on 2.0.1 (rounds down, overflows) |
| T31 | `SleepFor(Time(5))` sleeps 5..6 ticks, `SleepFor(Time(0))` 0 (2.1.0 API; SKIP before) |
| T19 | heap-free build types only: no dynamic RTOS allocation (see "Heap-free proof"); no RESULT line in builds with a heap |
| T16s | `SignalChannel::putFromISR()` to a receiver blocked in `input()` releases it, or returns false. (Today it returns true and the receiver stays blocked.) |
| T16n | `KeepNewest` rendezvous (`SamplingChannel`): `output()` while a reader waits in an ALT delivers the value. (Today the reader's `select()` returns the channel with its destination unchanged, and the value is dropped.) |
| T16a | *sweep*, rendezvous `putFromISR()` against a plain reader entering `input()`. The task path locks with the mutex, the ISR path with BASEPRI; an ISR between the two stores of `registerWaitingTask()` (`waiting_in_task`, then `non_alt_in_data_ptr`) sees a waiting reader with no destination. The copy is skipped (v1.0.0 checks the pointer), `putFromISR()` returns true and wakes the reader, which returns without the value. The second store then leaves a stale destination pointer in the channel. Correct: `putFromISR()` true → the reader has the value; false → the runner's kick value. **Passes on both FVPs and in the DK-E8's regular sweep**, whose steps (~10 instructions) miss the 1–2-instruction window. On the DK-E8 a cycle-step sweep hits it (`docs/hardware_results_dk_e8.md`): at `-O0` the trial fails as described, and afterwards the run hangs (next rendezvous on the same channel) or overwrites the runner's stack (fresh channel per failure); at `-O2` the board stops silently in the race region. No run showed a copy through a null pointer. A `HardFault_Handler` in the test file reports CFSR/HFSR/BFAR and ends the run if a fault happens |
| T15 | *sweep*, ALT-vs-ALT rendezvous: the ALT writer (the victim) completes the transfer in `activate()` and wakes the ALT reader only after releasing the mutex. If the reader has meanwhile taken X and started a new `select()`, the late flag lands in the new round. Per trial, **PHANTOM** = C returned with its destination unchanged, **SILENT** = X returned although C's data was written. This is the implementation form of CSP-M assertion 23 |

### T2 method

The test build interposes on the CMSIS-RTOS2 API: with armlink's `$Sub$$`/`$Super$$` patching (Arm
Compiler), or with GNU ld's `-Wl,--wrap=` (GCC; the flags are in the harness cproject). It covers:
`osThreadFlagsSet`, `osEventFlagsSet`, `osSemaphoreRelease/Acquire`, `osMessageQueuePut/Get/GetCount/GetSpace`
and `osMutexAcquire/Release`. Every call from the library therefore passes a check that counts calls made
with `BASEPRI != 0`. The map file lists the wrappers, and the disassembly shows the library's call sites
resolved to them.

The same mechanism counts `osTimerNew()` calls for T6, and makes one `osThreadNew()` call fail for T28
(GCC: `-Wl,--wrap=osTimerNew,--wrap=osThreadNew` in the harness cproject too).

The workload drives every notification path:
- an ALT reader woken by `output()` and by `putFromISR()`;
- an ALT writer woken by `input()`;
- a KeepNewest overwrite plus an ISR write;
- a rendezvous `putFromISR()` (1.x only; 2.0 has no ISR path into rendezvous channels);
- 2.0: the rendezvous and signal task paths listed in the T2 row.

### Phase-sweep method (T1a, T1b, T3, T3i, T13, T13b, T15, T16a)

1. The runner is the higher-priority aggressor. It aligns to a tick edge E0, releases the victim, and
   sleeps until the next edge E1.
2. The victim spins `k` iterations, then performs the operation under test.
3. At E1 the runner preempts the victim wherever it is and performs the conflicting operation. In T3i it
   pends the software interrupt, whose handler performs the conflicting operation.
4. A binary search finds the boundary `kb` where the aggressor starts acting before the victim's operation.
   Each of two sweeps then covers `k = kb−1500 … kb+1000` in steps of one iteration (about 10 cycles), so
   every instruction offset of the victim's operation is hit. The boundary search can land about 600
   iterations low, hence the wide upper range; in v1.0.0 the bug windows lay at `kb−60 … kb+190`.
5. A hang counts as a lost wakeup only if data or space is really available when the ACK times out (30
   ticks). One successor operation then rescues the victim, so the sweep continues.
6. A sweep verdict needs both regimes (EARLY and LATE trials) to be present.

## Test-harness notes

- Worker threads have 1 KB static stacks and the runner has 8 KB. T5's 32 KB static channel must fit the
  harness's 124.5 KB `RW_RAM0`.
- T15 priorities: runner > ALT reader (target) > ALT writer (victim). `check()` waits 2 ticks so that the
  target has finished before its log is read.
- Test threads are never deleted. Threads that are expected to hang (on v1.0.0) stay blocked forever.
- `csp4cmsis_fatal_error()` is overridden: the test records the message and parks the calling thread.
- RTX5: `osRtxErrorNotify()` is overridden to print the error code before halting.
- `results/2026-09-26_fvp_run.txt` is the output of the earlier, v1.0.0-only analysis suite (commit
  `1b757e1`), kept for reference.
