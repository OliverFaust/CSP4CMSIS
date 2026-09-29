# CSP4CMSIS 2.0: hardware results, Alif DK-E8 (stage 1)

**Date:** 2026-09-29. **Plan:** `docs/hardware_test_plan.md`, stage 1 and the hardware-only checks.
**Board:** Alif DevKit-E8, AE822FA0E5597LS0 rev A1, RTSS-HP (Cortex-M55) at 400 MHz, I- and D-cache on,
all code in ITCM. **Libraries:** 2.0 = `buffered-channel-v2` @ `c60665d`; v1.0.0 = `a789d2a`.
**Harness:** Alif-DK-E8-CSP4CMSIS, branch `csp4cmsis-2.0-hwtest` @ `e879866` (local, not pushed),
`csp4cmsis_hwtest/`: the FVP suite (`tests/fvp_sse300/bc_tests.cpp` @ `dff8277`) with platform macros
only, RTX5 5.9.1 and FreeRTOS 11.3.0 (ARM CMSIS-FreeRTOS adapter), tick 1 kHz, SWI = CANFD0 (IRQ 104)
at priority 192, `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` = 128 (8 priority bits). One log per run in
`csp4cmsis_hwtest/results/`, each naming the library commit.

## Regression suite

| Library | Backend | Toolchain | Result | Same as FVP? |
|---|---|---|---|---|
| v1.0.0 | RTX5 | AC6 6.24 | PASS=8 FAIL=18 SKIP=2 | yes: the FVP's 17 FAILs plus T18 (added since); SKIP adds T17 |
| 2.0 | RTX5 | AC6 | PASS=24 FAIL=0 SKIP=0 REPLACED=4 | yes |
| 2.0 | RTX5 | GCC 14.2.1 | PASS=24 FAIL=0 SKIP=0 REPLACED=4 | yes |
| 2.0 | FreeRTOS | AC6 | PASS=24 FAIL=0 SKIP=0 REPLACED=4 | yes |
| 2.0 | FreeRTOS | GCC | PASS=24 FAIL=0 SKIP=0 REPLACED=4 | yes |
| 2.0 | RTX5-NoHeap | AC6 | PASS=25 FAIL=0 SKIP=0 REPLACED=4, RTOS heap 0 B | yes |
| 2.0 | RTX5-NoHeap | GCC | PASS=25 FAIL=0 SKIP=0 REPLACED=4, RTOS heap 0 B | yes |
| 2.0 | FreeRTOS-NoHeap | AC6 | PASS=25 FAIL=0 SKIP=0 REPLACED=4; allocator not linked | yes |
| 2.0 | FreeRTOS-NoHeap | GCC | PASS=25 FAIL=0 SKIP=0 REPLACED=4; allocator trap calls 0 | yes |

- All at `-O0`. 2.0: every sweep covers both regimes with BUG=0 and ANOMALY=0; T13/T13b 0 spins; T15 0
  bad trials. RTOS heap used by the harness is identical to the FVP (280/40/96/104 B; 0 B heap-free).
- Two runs of the same v1.0.0 image gave identical kb values and BUG ranges.

## Sweeps: no recalibration needed

- One spin iteration is about 13.1 cycles (AC6) or 14.1 cycles (GCC), so kb is about 30 600 (AC6) or
  28 400 (GCC) per 1 ms tick; the swept range `kb-1500 .. kb+1000` is unchanged from the FVP.
- On v1.0.0 the sweeps that fail on the FVP fail here: T1a, T1b (BUG k≈30 320..30 544), T3 (k≈30 331..30 563),
  T15 (k≈29 977..30 423; 172 bad trials), T3i (hangs every trial, as on the FVP).
- T13 and T13b pass on v1.0.0, on the FVP and here. They detect a livelock that existed only during 2.0
  development (`d39835b`, fixed in `a85e17d`), so v1.0.0 cannot be their positive control.
- T16a passes on v1.0.0 in the regular sweep (FVP and board): see below.
- kb is much more stable than on the FVP (spread ≤ 15 iterations per configuration, FVP up to ~600).

## Hardware-only checks

### 1. T16a with a cycle-step sweep, v1.0.0 (RTX5, AC6)

Every k of `kb-1500 .. kb+1000` is also run with 0..31 extra NOPs before the operation, and the ISR records
the victim's preempted PC (from its RTX5 context).

- **FAIL, as expected.** BUG trials occur only with the victim preempted at `registerWaitingTask()+0x30`
  (`ldr`) or `+0x32` (`str non_alt_in_data_ptr`, not yet executed), i.e. after `str waiting_in_task` (+0x2e)
  and before the second store.
  That is the race of `BUFFERED_CHANNEL_ANALYSIS.md`: `putFromISR()` returns true, the reader returns
  without the value. First hit at k=30 534 with 30 NOPs; four hits at k=30 533..30 536.
- The regular sweep steps about 13 cycles and never lands in this 2-instruction window, which is why T16a
  passes there (and on the FVP).
- Consequences on the board: with one channel for the whole sweep, the run hangs after the first BUG (the
  second store lands after the ISR has cleared the registration, and the next rendezvous blocks). With a
  fresh channel after each BUG, the runner's local variables are overwritten after the fourth BUG and the
  run stops. The copy itself is null-guarded in v1.0.0 (`alt_channel_sync.cpp:25`), so no HardFault; the
  FVP README's "copies through a null pointer" describes the intended failure, not this code.
- 2.0 is not affected: rendezvous channels have no ISR path (compile check `neg_rendezvous_isr*.cpp`).

### 2. Masked-copy latency, 2.0 (RTX5, AC6)

`putFromISR()` cost, from the SWI handler (DWT cycle counter, 256 samples, 1 cycle = 2.5 ns):

| Element | KeepNewest into a full channel (masked copy only) | Block, reader waiting (copy + wake) |
|---|---|---|
| 4 B | 158 cycles (0.40 µs) | 918 avg, 1554 max |
| 64 B | 211 cycles (0.53 µs) | 969 avg, 1019 max |
| 1024 B* | 511 cycles (1.28 µs) | 1240 avg, 1293 max |

\* with `CSP4CMSIS_ISR_MAX_ELEMENT_SIZE=1024` (default 64).

Interrupt latency while a thread performs back-to-back `putFromISR()` copies: a DWT cycle-count match
raises DebugMonitor (priority 64) at a chosen cycle, which pends a probe interrupt; the probe records pend
→ entry (256 random trigger points per case; the floor of 64 cycles is the same with plain copies):

| Element | Probe above the threshold (priority 64) | Probe below the threshold (priority 160) | Added by the masked copy (max) |
|---|---|---|---|
| 4 B | 64 / 64 / 64 (min/avg/max) | 64 / 78 / 128 | 64 cycles (0.16 µs) |
| 64 B | 64 / 64 / 64 | 64 / 99 / 181 | 117 cycles (0.29 µs) |
| 1024 B | 64 / 64 / 64 | 64 / 227 / 481 | 417 cycles (1.04 µs) |

- Above the threshold: unaffected. Below it: the delay grows with the element and stays below the cost of
  one masked `putFromISR()`. The 64-byte case adds at most 0.29 µs, well under the RTOS part of a Block
  `putFromISR()` (semaphore release and wake, about 700 cycles).
- These numbers can replace the FVP-based estimate in `Documentation/CSP4CMSIS_Configuration.md` §4 (not
  changed here).
- Method note: DebugMonitor itself cannot be the probe. A DWT debug event that arrives while DebugMonitor
  is masked is dropped, not held pending (first attempt: up to 194 of 256 triggers lost).

### 3. Soak, 2.0 (RTX5, AC6)

The whole suite in a loop (`NVIC_SystemReset()` after each SUMMARY), about 85 s per pass, from 12:00 to
13:16 (the hour counted from the manual RESET at 12:12).

- 54 passes started, 52 completed: **all 52 `PASS=24 FAIL=0 SKIP=0 REPLACED=4`**. No FAIL, HardFault,
  RTOS error, ANOMALY, BUG or spin line in any pass. Heap used 280 B and runner stack minimum 7328 B in
  every pass.
- The two incomplete passes: pass 9 was cut short by the manual RESET (reset glitch byte after T13b), and
  pass 54 was still running when the log was taken.
- Not run: the pack-migration demo to 2 000 000 messages (the plan's other soak item).

## Differences from the FVP

- **T16a:** passes on the FVP and in the board's regular sweep; fails on v1.0.0 with the cycle-step sweep.
- **kb** stable to ≤ 15 iterations (FVP: up to ~600). Runner stack minimum free is 56 B lower in every
  configuration (e.g. 7328 vs 7384 B, RTX5 AC6).
- Everything else (results, heap use, T19 details) is identical.

## Not covered

- Positive control only for RTX5/AC6 (v1.0.0 FreeRTOS and GCC images build but were not run).
- `-O2`/`-Os` not run on the board. FreeRTOS only through ARM's CMSIS-FreeRTOS adapter.
- Stage 2 (NUCLEO-G474RE).
