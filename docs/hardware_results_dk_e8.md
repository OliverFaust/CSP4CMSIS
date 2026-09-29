# CSP4CMSIS 2.0: hardware results, Alif DK-E8 (stage 1)

**Date:** 2026-09-29. **Plan:** `docs/hardware_test_plan.md`, stage 1 and the hardware-only checks.
**Board:** Alif DevKit-E8, AE822FA0E5597LS0 rev A1, RTSS-HP (Cortex-M55) at 400 MHz, I- and D-cache on,
all code in ITCM. **Libraries:** 2.0 = `buffered-channel-v2` @ `c60665d` (`-O0` runs) and @ `73f46b7`
(`-O2`/`-Os` runs; same library sources, the commit only adds the harness's `BC_*` macros); v1.0.0 =
`a789d2a`. **Harness:** Alif-DK-E8-CSP4CMSIS, branch `csp4cmsis-2.0-hwtest` @ `45ce31f`
(<https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/csp4cmsis-2.0-hwtest/csp4cmsis_hwtest>;
the results are those of that commit, the branch head `62f1ef0` only adds licence information),
`csp4cmsis_hwtest/`: the FVP suite (`tests/fvp_sse300/bc_tests.cpp`) with platform macros, RTX5
5.9.1 and FreeRTOS 11.3.0 (ARM CMSIS-FreeRTOS adapter), tick 1 kHz, SWI = CANFD0 (IRQ 104) at priority
192, `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` = 128 (8 priority bits). One log per run in
`csp4cmsis_hwtest/results/`, each naming the library commit.

## Regression suite

| Library | Build types | Toolchains | Result | Same as FVP? |
|---|---|---|---|---|
| v1.0.0 | RTX5, `-O0` | AC6 6.24 | PASS=8 FAIL=18 SKIP=2 | yes: the FVP's 17 FAILs plus T18 (added since); SKIP adds T17 |
| 2.0 | RTX5, FreeRTOS; `-O0`, `-O2`, `-Os` | AC6, GCC 14.2.1 | **PASS=24 FAIL=0 SKIP=0 REPLACED=4 in all 12** | yes |
| 2.0 | RTX5-NoHeap, FreeRTOS-NoHeap; `-O0` | AC6, GCC | **PASS=25 FAIL=0 SKIP=0 REPLACED=4 in all 4**; RTOS heap 0 B (FreeRTOS: allocator not linked with AC6, 0 trap calls with GCC) | yes |

- 2.0: every sweep covers both regimes with BUG=0 and ANOMALY=0; T13/T13b 0 spins; T15 0 bad trials. RTOS
  heap used by the harness is identical to the FVP (280/40/96/104 B; 0 B heap-free).
- Two runs of the same v1.0.0 image gave identical kb values and BUG ranges.

## Sweeps: no recalibration needed

- One spin iteration takes 10 to 14 cycles depending on toolchain and optimisation, so kb is
  28 400..39 900 per 1 ms tick (`-O0` AC6 30 600, GCC 28 400; `-O2` AC6 39 900, GCC 36 200; `-Os` AC6
  39 900, GCC 33 200). The swept
  range `kb-1500 .. kb+1000` is unchanged from the FVP.
- On v1.0.0 the sweeps that fail on the FVP fail here: T1a, T1b (BUG k≈30 320..30 544), T3 (k≈30 331..30 563),
  T15 (k≈29 977..30 423; 172 bad trials), T3i (hangs every trial, as on the FVP).
- **T13 and T13b** pass on v1.0.0, on the FVP and here: they detect a livelock that existed only during 2.0
  development, so v1.0.0 cannot be their positive control. Their positive control is the development
  commit `d39835b` on the FVP (README of `tests/fvp_sse300/`, "History"), which failed both; `6920d1c`,
  the hash quoted for it elsewhere, is the same commit (identical tree) before the history rewrite of
  2026-09-27 and is on no branch. `d39835b` was not run on the board.
- T16a passes on v1.0.0 in the regular sweep (FVP and board): see below.
- kb is much more stable than on the FVP (spread ≤ 15 iterations per configuration, FVP up to ~600).

## Hardware-only checks

### 1. T16a with a cycle-step sweep, v1.0.0 (RTX5, AC6)

Every k of `kb-1500 .. kb+1000` is also run with 0..31 extra NOPs before the operation, and the ISR records
the victim's preempted PC (from its RTX5 context).

**`-O0`: FAIL, as expected.**
- BUG trials occur only with the victim preempted at `registerWaitingTask()+0x30` (`ldr`) or `+0x32`
  (`str non_alt_in_data_ptr`, not yet executed), i.e. after `str waiting_in_task` (+0x2e) and before the
  second store. That is the race of `BUFFERED_CHANNEL_ANALYSIS.md`: `putFromISR()` returns true, the reader
  returns without the value. First hit at k=30 534 with 30 NOPs; four hits at k=30 533..30 536.
- The regular sweep steps about 13 cycles and never lands in this 2-instruction window, which is why T16a
  passes there (and on the FVP).
- Consequences: with one channel for the whole sweep, the run hangs after the first BUG (the second store
  lands after the ISR has cleared the registration, and the next rendezvous blocks). With a fresh channel
  after each BUG, the runner's local variables are overwritten after the fourth BUG and the run stops.
  The copy itself is null-guarded in v1.0.0 (`alt_channel_sync.cpp:25`): no copy through a null pointer,
  no HardFault.

**`-O2`: FAIL (the board stops), location not identified.**
- At `-O2` the two stores are adjacent (`str` at +0x1a and +0x1c): the window is one instruction.
- In four runs the board stops silently at the same point, after the progress line for k=39 663 and
  below kb=39 913, i.e. in the region where the `-O0` race was hit (about kb−80). No BUG line is printed
  first.
- Diagnostics added run by run: a monitor thread (RTOS tick), a DebugMonitor watchdog at priority 0 driven
  by a DWT cycle match (runs even with BASEPRI raised), and a fault report that writes the UART registers
  directly. None of them prints. The core is therefore in lockup or executing corrupted code (code runs
  from writable ITCM); the exact instruction cannot be read without a debugger that supports the E8 (the
  installed J-Link software does not).
- 2.0 is not affected: rendezvous channels have no ISR path (compile check `neg_rendezvous_isr*.cpp`).

### 2. Masked-copy latency, 2.0 (RTX5, AC6)

`putFromISR()` cost, from the SWI handler (DWT cycle counter, 256 samples, 1 cycle = 2.5 ns), `-O0` / `-O2`:

| Element | KeepNewest into a full channel (masked copy only) | Block, reader waiting (copy + wake), avg |
|---|---|---|
| 4 B | 158 / 43 cycles | 918 / 258 cycles |
| 64 B | 211 / 97 cycles | 969 / 313 cycles |
| 1024 B* | 511 / 368 cycles | 1240 / 615 cycles |

\* with `CSP4CMSIS_ISR_MAX_ELEMENT_SIZE=1024` (default 64).

Interrupt latency while a thread performs back-to-back `putFromISR()` copies: a DWT cycle-count match
raises DebugMonitor (priority 64) at a chosen cycle, which pends a probe interrupt; the probe records pend
→ entry (256 random trigger points per case). The floor (64 cycles at `-O0`, 35 at `-O2`) is the same with
plain copies.

| Element | Probe above the threshold (prio 64), max | Probe below the threshold (prio 160), avg / max | Added by the masked copy (max − floor) |
|---|---|---|---|
| 4 B | 64 / 35 (= floor) | 78 / 128 at `-O0`; 41 / 59 at `-O2` | 64 cycles (0.16 µs) / 24 cycles (0.06 µs) |
| 64 B | 64 / 35 (= floor) | 99 / 181; 65 / 112 | 117 cycles (0.29 µs) / 77 cycles (0.19 µs) |
| 1024 B | 64 / 35 (= floor) | 227 / 481; 138 / 247 | 417 cycles (1.04 µs) / 212 cycles (0.53 µs) |

- Above the threshold: unaffected. Below it: the delay grows with the element and stays below the cost of
  one masked `putFromISR()`. The 64-byte case adds at most 0.29 µs, well under the RTOS part of a Block
  `putFromISR()` (semaphore release and wake: about 750 cycles at `-O0`, 220 at `-O2`).
- Recorded in `Documentation/CSP4CMSIS_Configuration.md` §4.
- Method note: DebugMonitor itself cannot be the probe. A DWT debug event that arrives while DebugMonitor
  is masked is dropped, not held pending (first attempt: up to 194 of 256 triggers lost).

### 3. Soak, 2.0 (RTX5, AC6, `-O0`)

The whole suite in a loop (`NVIC_SystemReset()` after each SUMMARY), about 85 s per pass, from 12:00 to
13:36 (84 minutes after the manual RESET at 12:12).

- 69 passes started, 67 completed: **all 67 `PASS=24 FAIL=0 SKIP=0 REPLACED=4`**. No FAIL, HardFault,
  RTOS error, ANOMALY, BUG or spin line in any pass. Heap used 280 B and runner stack minimum 7328 B in
  every pass.
- The two incomplete passes: pass 9 was cut short by the manual RESET (reset glitch byte after T13b), and
  pass 69 was running when the console was closed.
- Not run: the pack-migration demo to 2 000 000 messages (the plan's other soak item).

## Runner stack margin

The runner (8192 B stack) reports its minimum free stack at the end of every run.

| | `-O0` | `-O2` | `-Os` |
|---|---|---|---|
| Board, lowest of the configurations | 6984 B (FreeRTOS AC6) | 6536 B (FreeRTOS GCC) | 6840 B (FreeRTOS GCC) |
| Board − Corstone-300 FVP, per configuration | −48 .. −56 B | −16 .. +32 B | −16 .. +40 B |

- The earlier statement "56 B lower in every configuration" holds only at `-O0` (48–56 B, 56 B for RTX5
  AC6); at `-O2`/`-Os` the board is sometimes higher.
- Impact: at most 56 B (0.7 % of the stack; about 6 % of the ~0.9 KB the runner uses at `-O0`). The lowest minimum on the board is
  6536 B free (80 %). No change is needed. The difference comes from the board's own code on the runner's
  paths (most likely the `printf` path through the Alif USART driver; not isolated), not from CSP4CMSIS,
  whose code is the same on both targets.
- Worker threads (1 KB stacks) do not report their high-water marks in the suite; they are unchanged from
  the FVP harness and no stack-overflow report occurred in any run.

## Differences from the FVP

- **T16a:** passes on the FVP and in the board's regular sweep; fails on v1.0.0 with the cycle-step sweep.
- **kb** stable to ≤ 15 iterations (FVP: up to ~600).
- Runner stack: see above.
- Everything else (results, heap use, T19 details) is identical.

## Not covered

- Positive control on the board only for RTX5/AC6 (v1.0.0 FreeRTOS and GCC images build but were not run).
  On the MPS2 Cortex-M4 FVP it ran for both backends, AC6 `-O0` and GCC `-O2` (`tests/fvp_sse300/README.md`).
- FreeRTOS only through ARM's CMSIS-FreeRTOS adapter; heap-free builds only at `-O0`.
- Stage 2 (NUCLEO-G474RE, ST's CMSIS-RTOS2 wrapper): not needed for Armv7E-M coverage, which the MPS2
  Cortex-M4 FVP now provides; ST's wrapper remains unverified.
