# CSP4CMSIS 2.0: hardware test plan

**Status:** plan only; nothing has run on hardware yet. All 2.0 results so far come from the Corstone-300
FVP (`tests/fvp_sse300/`).

## What hardware must add

The FVP is instruction-accurate, not cycle-accurate. Some things it cannot show:
- **Interrupt timing at instruction granularity.** The FVP appears to take interrupts only between
  translated blocks (not verified). T16a's race window, 1–2 instructions, was never hit there.
- **Real interrupt latency** of the masked element copy (`putFromISR()` and every buffered operation copy
  with BASEPRI raised), and the effect of `DSB; ISB` after the BASEPRI write.
- **Different silicon, RTOS integrations and priority layouts:**
  - Alif E8 has 8 priority bits and the CMSIS-RTX 5.9.1 build of RTX5;
  - STM32G4 has 4 priority bits, Armv7E-M, and ST's own CMSIS-RTOS2 wrapper over FreeRTOS, not ARM's
    CMSIS-FreeRTOS adapter.
- **Long runs** (hours) instead of 130 s of simulated time.

## Stage 1: Alif DK-E8, M55_HP core, Keil RTX5 (first)

**Setup:**
- **Library:** the 2.0.0 pack built from `buffered-channel-v2` (`OliverFaust.CSP4CMSIS.2.0.0.pack`,
  local, not published), installed into the DK-E8 project's pack root.
- **Project:** a new `csp4cmsis_regression` project next to `csp4cmsis_alt_test`, with the same device
  (`:M55_HP`), RTX5 component, `CSP4CMSIS_RTOS2_BACKEND_RTX5`, `CSP4CMSIS_STATIC_ALLOCATION` and
  `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY: 128` (unshifted; 8 priority bits, so BASEPRI = 128).
- **Toolchains:** Arm Compiler 6.24 and GCC 14.2.1, `-O0` and `-O2`, so 4 images.
- **Heap-free variant:** `OS_DYNAMIC_MEM_SIZE=0`, `OS_MUTEX_OBJ_MEM=1`, `OS_MUTEX_NUM=8` (as on the FVP),
  for T19.

## Stage 2: one FreeRTOS board, NUCLEO-G474RE

This is the board the book uses, and it tests the most different integration: Cortex-M4F (Armv7E-M),
STM32Cube's CMSIS-RTOS2 wrapper, and STM32CubeIDE/GCC.
- **Library:** the 2.0 sources vendored into `lib/csp4cmsis/`, like the book projects (the IDE does not
  use packs).
- **Defines:** `CSP4CMSIS_RTOS2_BACKEND_FREERTOS`, `CSP4CMSIS_STATIC_ALLOCATION`,
  `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY: 5` (4 priority bits → BASEPRI 0x50; must match the project's
  `configMAX_SYSCALL_INTERRUPT_PRIORITY`).
- **Check first:** ST's `cmsis_os2.c` (2013–2020 base) against the assumptions in `csp_rtos_static.h`:
  - the static timer control block (callback wrapper stored after `StaticTimer_t`?);
  - `osSemaphoreNew()`'s static path;
  - thread flags implemented with task notification index 0.

  T6/T17 (heap use) are the run-time check.
- **Alternative** if Stage 2 should stay on ARM's adapter: HimaxWE2 (Cortex-M55, CMSIS-FreeRTOS). It
  would not cover Armv7E-M.

## Test inventory

| Tests | On hardware | Porting needed |
|---|---|---|
| Compile checks (`tests/compile_checks/`, 17 probes) | host only, unchanged | run `run_checks.py` against the board project's `compile_commands.json` |
| T0, T4a/b/c, T6, T7a, T8, T9, T10, T11, T12, T14, T15s, T17, T18 | as-is | none: CSP4CMSIS and CMSIS-RTOS2 calls only |
| T1a, T1b, T3, T13, T13b, T15 (phase sweeps) | as-is | none: the boundary search calibrates itself per build. Expect different `kb` values and different BUG windows (if any) |
| T3i, T2 (ISR paths) | port | the software-interrupt source: replace `I2S_IRQn` with an unused interrupt of each device, priority numerically ≥ the threshold (DK-E8: e.g. 192; G474: 6) |
| T2 (RTOS calls with BASEPRI raised) | port | interposition works with AC6 (`$Sub$$`) and GCC (`-Wl,--wrap=…`); add the ten `--wrap` flags to the STM32CubeIDE linker settings |
| T5 (large channel) | port (G474) | 8200 × 4 B does not fit into 128 KB beside the rest: reduce to a size that still exceeds the RTOS heap (e.g. 4096 × 4 B with a 12 KB heap) |
| T19 (heap-free) | Stage 1 variant | Stage 2 only if ST's wrapper builds with `configSUPPORT_DYNAMIC_ALLOCATION 0` (not yet checked) |
| T15i, T16a, T16s, T16n | REPLACED on 2.0 (compile checks) | see the hardware-only check below for T16a |
| Harness infrastructure | port | UART retarget (DK-E8: board UART; G474: USART2 over the ST-LINK virtual COM port); no EOT needed (the run ends after the SUMMARY line); `HardFault_Handler` report; heap query (RTX5 as on the FVP; ST FreeRTOS `heap_4` API as on the FVP) |
| RAM budget (G474, 128 KB) | port | the FVP image uses about 125 KB. Reduce the RTOS heap to 8–12 KB, runner stack to 4 KB, worker stacks where the high-water marks allow (report `osThreadGetStackSpace`), and run the suite in two halves if needed |

## Hardware-only checks

1. **T16a on hardware (positive control for a defect the FVP could not reach).** Build the harness
   against **v1.0.0** (which still has rendezvous `putFromISR()`) with AC6 `-O0`, where
   `registerWaitingTask()`'s two stores are one instruction apart.
   - Sweep the ISR arrival point in 1-cycle steps: trigger the software interrupt from a timer compare
     computed from `DWT->CYCCNT`, instead of the tick edge (≈10-cycle steps).
   - **Expected:** FAIL on v1.0.0 (value lost, or a HardFault from the copy through a null pointer).
   - **Reading:** FAIL confirms the defect and explains the FVP result. PASS is **inconclusive** (report
     the swept range). It does not affect 2.0, where the path no longer exists (compile check
     `neg_rendezvous_isr*.cpp`).
2. **Masked-copy latency.** With `DWT->CYCCNT`, measure the BASEPRI-masked interval of
   `IsrChanout::putFromISR()` for 4, 64 and (with the limit raised) 1024-byte elements. Also measure the
   latency of an interrupt *above* the threshold (must be unaffected) and one *below* it (grows with the
   copy).
   - **Pass:** the 64-byte case stays in the range of an RTOS queue operation on the same board. Record
     the numbers in `CSP4CMSIS_Configuration.md` §4, replacing the FVP-based estimate.
3. **Soak.** The pack-migration demo (two senders, one fair-select receiver) to completion
   (2,000,000 messages; about 10 G instructions at about 5,000 per message on the FVP, i.e. under a
   minute at 400 MHz), plus the regression suite in a loop for ≥ 1 hour.
   - **Pass:** `SUCCESS` line; no `FAIL`, fatal error or HardFault in any iteration.

## Result collection

- **Capture:** a host script (pyserial) records the UART log to
  `tests/hw_<board>/results/<date>_<config>.txt`. The format is the same `RESULT <id>: …` / `SUMMARY` as
  on the FVP, so the existing parsing applies.
- **Header per log:** board and silicon revision, core clock, toolchain and flags, library commit or
  pack version, RTOS version, and the priority-threshold value. Keep the map file next to each log.
- **Repeat:** each configuration runs twice. On hardware the sweeps are not expected to be bit-identical;
  the verdicts must be.
- **Summary:** a table per stage like the FVP README's, committed with the logs.

## What counts as pass

| Check | 2.0 | v1.0.0 (positive control, Stage 1 only) |
|---|---|---|
| Regression suite | every test PASS or REPLACED, no FAIL; T13/T13b 0 spins; T15 0 bad trials; every sweep covers both regimes (EARLY and LATE) | the v1.0.0 FVP failures reappear (T1a, T1b, T3, T3i at least), which shows the sweeps are sensitive on this hardware |
| T16a (hardware-only, 1-cycle sweep) | n/a (REPLACED) | FAIL expected; PASS = inconclusive |
| Heap-free variant (T19) | PASS, 0 B RTOS heap | — |
| Compile checks | all 17 pass with the board's flags | — |
| Masked-copy latency | measured and documented; above-threshold interrupt unaffected | — |
| Soak | no failure | — |

## Order and effort (estimate)

1. **Stage 1, DK-E8:** port the harness (IRQ, UART, project), about 1 day; runs and v1.0.0 control,
   about 1 day; T16a cycle sweep and latency, about 1 day.
2. **Stage 2, NUCLEO-G474RE:** ST wrapper review and RAM trimming, about 1–2 days; runs, about 0.5 day.
