# CSP4CMSIS 2.0.1: hardware results, NUCLEO-G474RE (stage 2)

**Date:** 2026-10-03. **Plan:** `docs/hardware_test_plan.md`, stage 2. **Board:** NUCLEO-G474RE
(STM32G474RET6, Cortex-M4F, Armv7E-M, 170 MHz, ST-LINK-V3), 1 kHz tick. **Stack:** STM32CubeMX 6.17.0,
STM32Cube FW_G4 V1.6.3 (FreeRTOS 10.3.1, ST's `CMSIS_RTOS_V2` wrapper), STM32CubeIDE 2.1.0 (GNU Tools for
STM32 14.3.rel1). **Library:** `release-2.0.1` @ `c69059f` (library sources as `5b1239c`); controls:
`v2.0.0` (`eed5b94`) and v1.0.0 (`a789d2a`). **Projects:** made as in
`Documentation/CSP4CMSIS_STM32CubeIDE.md` (`tests/hw_nucleo_g474/README.md`), FreeRTOS timer task
priority at CubeMX's default (2) unless stated. **Logs:** `tests/hw_nucleo_g474/results/2026-10-03_*`,
each with the ELF's SHA-256 (flashed with STM32CubeProgrammer, VCP logged by `nucleo_run.py`).

## Results

| Configuration | Result |
|---|---|
| **2.0.1**, priority 2, `-O0` / `-Os` | **PASS=29 FAIL=0 SKIP=0 REPLACED=4** both |
| 2.0.1, priority 55, `-O0` / `-Os` | PASS=29 FAIL=0 both |
| 2.0.1, **heap-free** (no RTOS dynamic allocation), `-O0` / `-Os` | **PASS=30 FAIL=0** both; T19: 0 allocator calls; heap used 0 B |
| Guide example (`csp_app.cpp`), priority 2 | `2000 messages, 0 errors, 0 timeouts: PASS` (before and after a CubeMX regeneration) |
| **Positive control 2.0.0**, priority 2, `-O0` | stops after T17 (as on the FVP; T6 FAIL: 201 RTOS timers) — see below |
| **Positive control v1.0.0**, priority 55, `-O0` | PASS=12 FAIL=18 SKIP=3: **the same 18 failures as on the FVP** (MPS2, ST's wrapper) |

- **Sweeps (2.0.1):** 84 sweep lines, all BUG=0 ANOMALY=0; T13/T13b 0 spins; T15 0 bad trials.
- **Every test, every 2.0.1 configuration:** T0, T2, T4a/b/c, T5, T6, T7a, T8–T12, T14, T15s, T17, T18,
  T1a, T1b, T3, T3i, T13, T13b, T15, T20–T24 PASS (T19 PASS in the heap-free builds); T15i, T16s, T16n,
  T16a REPLACED (compile checks).

### Timeouts (2.0.1, identical in all six suite runs)

| Test | Board |
|---|---|
| T20 accuracy | 1, 3, 10 ticks: selected after exactly 1, 3, 10 ticks (5 trials each, varied phase) |
| T21 timeout 10 vs channel after 8..12 ticks | 8, 9: channel 3/3; 10, 11, 12: timeout 3/3, item then read; one transfer per trial |
| T22 timeouts (30, 5, 15) | guard 2 (5 ticks) after 5 ticks |
| T23 zero timeout | selected after 0 ticks; a ready channel listed first wins (value 4242, one transfer) |
| T24 stale wakeup every tick, timeout 10 | timeout after 10 ticks |
| T6 | 0 B heap per `RelTimeoutGuard`, 200 constructions: 0 allocations, **0 RTOS timers** |

### Positive control 2.0.0 (priority 2)

The run stops after T17 (no further output), where the FVP control ends in a HardFault. Read from the
running core without reset (`STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -coreReg`): thread mode,
PC in `vPortFree()` at `heap_4.c:281`, `configASSERT(pxLink->pxNextFreeBlock == NULL)`, called from
`prvProcessReceivedCommands()` (`timers.c:871`): the timer task processing a queued delete of a
`RelTimeoutGuard` timer whose storage has been reused (T6's 200 guards on the runner's stack), so the
status byte reads "dynamically allocated" and FreeRTOS frees a stack address. CubeMX's `configASSERT`
disables interrupts and spins, hence no fault report. Same defect as on the FVP (2.0.0 known issue,
item 1); the symptom differs only because CubeMX's configuration asserts before the list corruption
faults. Register dump appended to the log.

### Positive control v1.0.0: the race sweeps detect the defects on this board

| Sweep | kb (iterations/tick) | v1.0.0 |
|---|---|---|
| T1a, T1b | 11 705–11 709 | BUG 15–16 (k 11 617..11 635) |
| T3, T3i | 11 723 | BUG 49 (k 11 600..11 648) |
| T15 | 11 723 | BUG 132–135 (k 10 718..11 409) |
| T13, T13b, T16a | 11 723 | pass, as on the FVP (T13/T13b: a 2.0-development livelock, positive control is `d39835b`; T16a: the window needs a cycle-step sweep, `docs/hardware_results_dk_e8.md`) |

No sweep needed recalibration: the boundary search finds kb by itself (about 11 700 iterations per 1 ms
tick at `-O0`, 12 800–15 100 at `-Os`) and the swept range `kb-1500 .. kb+1000` contains the race
windows.

## Stack and heap

| | `-O0` | `-Os` | MPS2 FVP, ST's wrapper, GCC `-O0` |
|---|---|---|---|
| Runner (8192 B) minimum free | 6976 B | 6736 B (heap-free 6752 B) | 7080 B (heap-free 7116 B) |
| RTOS heap used | 2328 B | 2336 B | 104 B |
| RTOS heap used, heap-free build | 0 B (no allocator in the image) | 0 B | 0 B |

- **Heap:** CSP4CMSIS uses none (T6, T17: 0 allocations). The 2.3 KB is CubeMX's `defaultTask`, created
  dynamically (512-word stack plus TCB); the FVP harness creates no such task.
- **Heap-free build** (`csp4cmsis_g474_tests_noheap`): `defaultTask` static (CubeMX: Tasks and Queues,
  Allocation Static), `configSUPPORT_DYNAMIC_ALLOCATION 0` in `FreeRTOSConfig.h` USER CODE Defines,
  `heap_4.c` excluded from the build, counting traps for `pvPortMalloc()`/`vPortFree()`. GCC's
  `--gc-sections` removed the traps and every user of them (`osTimerNew()`, `osThreadEnumerate()`):
  nothing in the image references an allocator.
- **Runner stack:** 104 B (`-O0`) less free than on the FVP; most likely the board's `printf` path (BSP COM,
  HAL UART) and newlib's reentrancy (`configUSE_NEWLIB_REENTRANT 1`), not isolated. Worker stacks (1 KB) do not report
  their marks; no stack overflow occurred.
- **RAM:** 121 300 B of 131 072 B for the suite image (`.data` + `.bss`, incl. T5's 32 KB channel and the
  16 KB FreeRTOS heap); no trimming was needed.

## CubeMX regeneration (guide project)

GENERATE CODE again on the unchanged `.ioc`: CubeMX regenerated 37 files and rewrote `.cproject`; the
project afterwards was byte-identical (no change in git), so `lib/csp4cmsis/` (29 files), the source
folder `lib/csp4cmsis/src`, the G++ include path, the four defines, GNU++17 and the `USER CODE` calls in
`main.c` all survived. A clean rebuild gave the same ELF (SHA-256 `a1c9d35e…`); the example passed again.
(Earlier regenerations with changed FreeRTOS settings: `docs/results_nucleo_g474.md`.)

## Differences from the FVP

- **2.0.0 control:** configASSERT trap in the timer task instead of a HardFault (same defect, see above).
- **kb:** ~11 700 iterations per tick (170 MHz, 1 kHz) against ~20 800 on the MPS2 FVP (25 MHz,
  100 Hz); the sweeps calibrate themselves.
- **Heap and stack:** CubeMX's dynamic `defaultTask` (2.3 KB heap); runner stack 104 B lower.
- Everything else (test results, timeout timings T20–T24, heap-free result) is the same as on the FVP.

## Not covered

- The hardware-only checks and the soak of stage 1 (not repeated on this board).
- Arm Compiler 6 and RTX5 on this board (the projects are STM32CubeIDE/GCC/FreeRTOS, as the book's).
