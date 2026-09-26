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

| Library | FreeRTOS | RTX5 |
|---|---|---|
| v2, `buffered-channel-v2` @ `a34d608` | PASS=19 FAIL=0 SKIP=0 | PASS=19 FAIL=0 SKIP=0 |
| v1.0.0 @ `a789d2a` (regression baseline) | PASS=6 FAIL=12 SKIP=1 | PASS=6 FAIL=12 SKIP=1 |

On v1.0.0, only T0, T8, T11, T12, T13 and T13b pass. T9 needs a v2-only hook. The intermediate commit
`6920d1c` failed T13/T13b (a livelock fixed in `a34d608`).

## Harness

- **Project:** `helloworld_sse300` in the `arm_fvp_helloworld` repository, local branch
  **`csp4cmsis-wt-tests`**. That repository's `origin` is not ours: never push it.
- **Target:** Corstone-300 FVP (Fast Models 11.28.32, `FVP_Corstone_SSE-300_Ethos-U55`), Cortex-M55.
  Arm Compiler 6.24 at `-O0`, CMSIS-Toolbox 2.14.1.
- **Build types** (`hello.csolution.yml`):

  | Build type | CSP4CMSIS backend define | RTOS components |
  |---|---|---|
  | `.FreeRTOS` | `CSP4CMSIS_RTOS2_BACKEND_FREERTOS` | CMSIS-RTOS2 FreeRTOS adapter + FreeRTOS 11.3.0; config in `RTE/RTOS/FreeRTOSConfig.h` (32 KB heap_4) |
  | `.RTX5` | `CSP4CMSIS_RTOS2_BACKEND_RTX5` | `ARM::CMSIS:RTOS2:Keil RTX5&Source` 5.9.1; `RTE/CMSIS/RTX_Config.h` |

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
cbuild hello.csolution.yml --packs --toolchain AC6 --rebuild          # both build types
for bt in FreeRTOS RTX5; do
  $FVP_BIN_DIR/FVP_Corstone_SSE-300_Ethos-U55 -a out/MPS3-Corstone-300/$bt/hello.axf \
      -C ethosu.num_macs=128 -f model_config_sse300.txt --simlimit 1200 --stat > run_$bt.txt &
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
| T2 | no CMSIS-RTOS2 call is made with BASEPRI raised (see "T2 method" below) |
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

### T2 method

The test build interposes on the CMSIS-RTOS2 API with armlink's `$Sub$$`/`$Super$$` patching:
`osThreadFlagsSet`, `osEventFlagsSet`, `osSemaphoreRelease/Acquire`, `osMessageQueuePut/Get/GetCount/GetSpace`
and `osMutexAcquire/Release`. Every call from the library therefore passes a check that counts calls made
with `BASEPRI != 0`. The map file lists the wrappers, and the disassembly shows the library's call sites
resolved to them.

The workload drives every notification path:
- an ALT reader woken by `output()` and by `putFromISR()`;
- an ALT writer woken by `input()`;
- a KeepNewest overwrite plus an ISR write;
- a rendezvous `putFromISR()`.

### Phase-sweep method (T1a, T1b, T3, T3i, T13, T13b)

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
- Test threads are never deleted. Threads that are expected to hang (on v1.0.0) stay blocked forever.
- `csp4cmsis_fatal_error()` is overridden: the test records the message and parks the calling thread.
- RTX5: `osRtxErrorNotify()` is overridden to print the error code before halting.
- `results/2026-09-26_fvp_run.txt` is the output of the earlier, v1.0.0-only analysis suite (commit
  `21c0e09`), kept for reference.
