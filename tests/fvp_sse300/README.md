# BufferedChannel / ALT regression suite (Corstone-300 FVP)

`bc_tests.cpp` is the regression suite behind `BUFFERED_CHANNEL_ANALYSIS.md` and the 2.0 BufferedChannel
work (`docs/CHANGES_2.0.md`). It is **test code**: it deliberately uses `csp::internal` classes, backend
heap queries and armlink symbol patching, and is not part of the pack.

The same source builds against:
- **either library generation:** v1.0.0 (`BufferedChannel<T, P>(capacity)`) or v2
  (`BufferedChannel<T, SIZE, P>`, detected via `CSP4CMSIS_BUFFERED_CHANNEL_API`);
- **either CMSIS-RTOS2 backend:** FreeRTOS 11.3.0 through `ARM::CMSIS-FreeRTOS`, or Keil RTX5 5.9.1
  through `ARM::CMSIS-RTX`.

Each test prints `RESULT <id>: PASS | FAIL | SKIP -- <property>`, where FAIL means the defect is present.
A `SUMMARY` line follows, and then EOT, which ends the FVP run.

## Current results (`results/`)

| Library | Configurations | Result |
|---|---|---|
| 2.0, `buffered-channel-v2`: OWRV rendezvous/signal channels (`fcc29c5`), C1/C2 API (`bd3965f`), static Barrier (`c109660`), migrated harness | **12**: Arm Compiler 6.24 and GCC 14.2.1 × `-O0`/`-O2`/`-Os` × FreeRTOS/RTX5 | **PASS=24 FAIL=0 SKIP=0 REPLACED=4 in all 12.** T13/T13b: 0 spins in every sweep; T15: 0 bad trials |
| 2.0, heap-free builds (see "Heap-free proof") | **4**: `FreeRTOS-NoHeap`, `RTX5-NoHeap` × AC6/GCC, `-O0` | **PASS=25 FAIL=0 SKIP=0 REPLACED=4** (T19 included); RTOS heap used: 0 B |
| v1.0.0 @ `a789d2a` (regression baseline; 26-test suite of `1ad3e11`) | AC6 `-O0`, FreeRTOS and RTX5 | PASS=8 FAIL=17 SKIP=1 on both |

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
- `6920d1c` failed T13/T13b (livelock);
- `a34d608` fixed it with a one-tick back-off;
- `dd954a9` replaced the back-off with semaphore-count readiness.

## Heap-free proof (`FreeRTOS-NoHeap`, `RTX5-NoHeap` build types)

Two extra build types of the harness (FVP test branch) disable RTOS dynamic allocation completely. The
whole suite runs on them with Arm Compiler 6 and GCC.

| Build type | RTOS configuration | What proves "no dynamic RTOS allocation" |
|---|---|---|
| `FreeRTOS-NoHeap` | `configSUPPORT_DYNAMIC_ALLOCATION=0`: FreeRTOS's dynamic-allocation API is not compiled; **no heap implementation is linked** (the Heap component is left out) | `pvPortMalloc`/`vPortFree` are defined only as counting traps (`noheap_stubs.c`), because the CMSIS-FreeRTOS adapter references them without checking the setting. **AC6:** the linker removes the traps, so nothing in the image references an allocator at all. **GCC:** the traps are linked and T19 reports 0 calls |
| `RTX5-NoHeap` | `OS_DYNAMIC_MEM_SIZE=0`: **no dynamic memory pool** (`osRtxInfo.mem.common == NULL`, `os_mem` absent from the image). Any object created without static memory would get NULL, which CSP4CMSIS treats as fatal | T19 checks that the pool is absent; the suite passing shows every object was created statically |

- **Common evidence:** T17 (50 constructions of `Channel` + `SignalChannel` + `Barrier`, no allocation);
  "heap used = 0 B" in the SUMMARY line; the CSP4CMSIS object files reference no `malloc`, `operator new`,
  `pvPortMalloc` or `osRtxMemoryAlloc`. (Sized `operator delete` is referenced by the deleting
  destructors of classes with virtual destructors, and is never called.)
- **Workarounds needed only because of the RTOS packages** (test branch; upstream issue draft:
  `docs/upstream/CMSIS-FreeRTOS_no_dynamic_allocation.md`, not filed):
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

- **Project:** `helloworld_sse300` in the `arm_fvp_helloworld` repository, local branch
  **`csp4cmsis-wt-tests`**. That repository's `origin` is not ours: never push it.
- **Target:** Corstone-300 FVP (Fast Models 11.28.32, `FVP_Corstone_SSE-300_Ethos-U55`), Cortex-M55.
  CMSIS-Toolbox 2.14.1.
- **Build types** (`hello.csolution.yml`): each backend at three optimisation levels.

  | Build types | CSP4CMSIS backend define | RTOS components |
  |---|---|---|
  | `.FreeRTOS`, `.FreeRTOS-O2`, `.FreeRTOS-Os` | `CSP4CMSIS_RTOS2_BACKEND_FREERTOS` | CMSIS-RTOS2 FreeRTOS adapter + FreeRTOS 11.3.0; config in `RTE/RTOS/FreeRTOSConfig.h` (32 KB heap_4) |
  | `.RTX5`, `.RTX5-O2`, `.RTX5-Os` | `CSP4CMSIS_RTOS2_BACKEND_RTX5` | `ARM::CMSIS:RTOS2:Keil RTX5&Source` 5.9.1; `RTE/CMSIS/RTX_Config.h` |

  - The plain types use the compiler default (`-O0`); the `-O2`/`-Os` types pass that flag verbatim
    (`misc`).
  - Toolchains: `--toolchain AC6` (Arm Compiler 6.24) or `--toolchain GCC` (GCC 14.2.1; set
    `GCC_TOOLCHAIN_14_2_1=/usr/bin`).
  - Output goes to `out/<target>/<build type>/<compiler>/`.

  `RTX_Config.h` is aligned with the FreeRTOS setup:
  - `OS_TICK_FREQ 100`
  - `OS_ROBIN_ENABLE 0`
  - `OS_TIMER_THREAD_PRIO 55`
  - `OS_THREAD_LIBSPACE_NUM 8`
  - `OS_STACK_WATERMARK 1`
- **Both builds:** `CSP4CMSIS_STATIC_ALLOCATION` and `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5`
  (BASEPRI 0xA0).
- **Timing:** the FVP tick runs at about 312.5 Hz on both backends. `core_clk.mul` is 100 MHz while the
  software assumes 32 MHz. The tests use tick counts only.

### Which library is under test

`hello.cproject.yml` loads CSP4CMSIS as a **local pack** from the symlink `./csp4cmsis_under_test`:

```yaml
    - pack: OliverFaust::CSP4CMSIS
      path: ./csp4cmsis_under_test
```

csolution reads `OliverFaust.CSP4CMSIS.pdsc` directly from the linked tree, so
`OliverFaust::CSP4CMSIS:Core` compiles that tree's sources. The installed `OliverFaust::CSP4CMSIS@1.0.0` in
`~/cmsis_packs` is **not** used, and nothing is registered with `cpackget`. The test source itself always
comes from this repository's working tree.

```sh
cd <arm_fvp_helloworld>/helloworld_sse300
ln -sfn ../../../../../../home/of6/src/CSP4CMSIS        csp4cmsis_under_test   # working tree (v2)
ln -sfn ../../../../../../home/of6/src/CSP4CMSIS-v1.0.0 csp4cmsis_under_test   # v1.0.0 regression baseline
#   (git -C ~/src/CSP4CMSIS worktree add --detach ~/src/CSP4CMSIS-v1.0.0 v1.0.0)
```

To verify which library was used:
`grep -o '/home/of6/src/CSP4CMSIS[^/]*/csp4cmsis/src/[a-z_]*.cpp' out/MPS3-Corstone-300/*/compile_commands.json`

## Run

```sh
source ../env.sh
export GCC_TOOLCHAIN_14_2_1=/usr/bin                                    # for GCC builds
cbuild hello.csolution.yml --packs --toolchain AC6 --rebuild           # all 6 build types
cbuild hello.csolution.yml --packs --toolchain GCC                     # all 6 build types
for d in out/MPS3-Corstone-300/*/*/; do
  img=$(ls $d/hello.axf $d/hello.elf 2>/dev/null | head -1)
  $FVP_BIN_DIR/FVP_Corstone_SSE-300_Ethos-U55 -a $img -C ethosu.num_macs=128 \
      -f model_config_sse300.txt --simlimit 1500 --stat > run_$(basename $(dirname $d))_$(basename $d).txt &
done; wait
```

- Each run takes about 130 s of simulated time. The v1.0.0 RTX5 run takes about 310 s, because T3i hangs
  on every trial there. That is about 5–10 minutes of wall-clock time.
- Two runs of the same image produce identical output (apart from telnet port numbers).

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
| T6 | `RelTimeoutGuard` and `Alternative` use no RTOS heap (200 constructions in a loop) |
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
| T19 | heap-free build types only: no dynamic RTOS allocation (see "Heap-free proof"); no RESULT line in builds with a heap |
| T16s | `SignalChannel::putFromISR()` to a receiver blocked in `input()` releases it, or returns false. (Today it returns true and the receiver stays blocked.) |
| T16n | `KeepNewest` rendezvous (`SamplingChannel`): `output()` while a reader waits in an ALT delivers the value. (Today the reader's `select()` returns the channel with its destination unchanged, and the value is dropped.) |
| T16a | *sweep*, rendezvous `putFromISR()` against a plain reader entering `input()`. The task path locks with the mutex, the ISR path with BASEPRI; an ISR between the two stores of `registerWaitingTask()` copies through a null pointer. Correct: `putFromISR()` true → the reader has the value; false → the runner's kick value. (Passes on the FVP; see "Current results".) A `HardFault_Handler` in the test file reports CFSR/HFSR/BFAR and ends the run if a fault happens |
| T15 | *sweep*, ALT-vs-ALT rendezvous: the ALT writer (the victim) completes the transfer in `activate()` and wakes the ALT reader only after releasing the mutex. If the reader has meanwhile taken X and started a new `select()`, the late flag lands in the new round. Per trial, **PHANTOM** = C returned with its destination unchanged, **SILENT** = X returned although C's data was written. This is the implementation form of CSP-M assertion 23 |

### T2 method

The test build interposes on the CMSIS-RTOS2 API: with armlink's `$Sub$$`/`$Super$$` patching (Arm
Compiler), or with GNU ld's `-Wl,--wrap=` (GCC; the flags are in the harness cproject). It covers:
`osThreadFlagsSet`, `osEventFlagsSet`, `osSemaphoreRelease/Acquire`, `osMessageQueuePut/Get/GetCount/GetSpace`
and `osMutexAcquire/Release`. Every call from the library therefore passes a check that counts calls made
with `BASEPRI != 0`. The map file lists the wrappers, and the disassembly shows the library's call sites
resolved to them.

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
  `21c0e09`), kept for reference.
