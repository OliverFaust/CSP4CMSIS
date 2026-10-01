# CSP4CMSIS 2.0.1: changes

Bug-fix release. The public API is unchanged; code that builds with 2.0.0 builds with 2.0.1. Evidence:
`docs/results_nucleo_g474.md` (FVP runs; NUCLEO-G474RE board runs pending at the time of writing) and the
model `docs/formal/alt_timeout_deadline.csp`.

## Fixed: ALT timeouts (`RelTimeoutGuard`)

In 2.0.0 a timeout guard was a CMSIS-RTOS2 timer. With FreeRTOS, a timer service task below a thread that
uses timeouts (STM32CubeMX's default priority 2) crashed or hung the application; the timer callback
could run after the guard was gone; a stale wakeup restarted the timer, so repeated wakeups could
postpone a timeout indefinitely; and on ST's STM32Cube wrapper every guard used 16 bytes of RTOS heap.
Details and the affected configurations: `docs/known-issues.md`.

2.0.1 implements timeouts without an RTOS timer. `select()` fixes a deadline when it starts
(`osKernelGetTickCount()`) and waits for its thread flags at most until the earliest deadline of its
enabled timeout guards; a new round never moves the deadline. Consequences:
- no timer object, callback or deferred timer command: no requirement on the RTOS timer service
  (`configUSE_TIMERS`, `configTIMER_TASK_PRIORITY`, `OS_TIMER_THREAD_PRIO`);
- no RTOS memory for timeout guards on any backend or adapter;
- a timeout of `d` ticks is selected after `d` ticks (`d + 1` at most), also across the 32-bit tick-count
  wrap; a zero timeout is selected at once without waiting; with several timeout guards the earliest
  deadline is selected; stale wakeups do not postpone it.

Verified before implementation with ProB (liveness under stale wakeups, a channel that becomes ready at
any time, several timeout guards, zero timeout, tick wrap; the 2.0.0 timer design and a per-round
deadline fail as they must), and by the regression suite (tests T6, T20-T24 below).

## Builds without CMSIS packs

- **`csp_critical.h`** includes `RTE_Components.h` only if it exists (`__has_include`). Without it
  (STM32CubeIDE, vendor SDK makefiles), `CSP4CMSIS_DEVICE_HEADER` names the device header, e.g.
  `"stm32g4xx.h"`; with neither, the build stops with an `#error`. Pack builds are unchanged.
- **Include path: `csp4cmsis/inc/` only**, in every build. The pdsc lists the headers as
  `category="other"`, so pack builds no longer add `csp4cmsis/inc/csp/` (where `csp/time.h` hid the C
  library's `<time.h>`); the Himax SDK fragment `csp4cmsis.mk` likewise.
- New guide: `Documentation/CSP4CMSIS_STM32CubeIDE.md` (STM32CubeMX/CubeIDE, step by step); configuration
  doc sections 7 (builds without packs) and 8 (timeouts); `docs/st_cmsis_rtos2_wrapper.md`.

## Internal changes (only code using `csp::internal` is affected)

- `internal::TimerGuard` is now `internal::TimeoutGuard` (no RTOS object); `csp_static_timer_storage_t`
  is removed from `csp_rtos_static.h`, which no longer includes FreeRTOS's `timers.h`.
- `internal::SkipGuard` (unused) is removed.
- `AltScheduler` records the start tick of `select()`.

## Pack

- `scripts/build_pack.py` builds byte-identical packs: two builds of the same commit, from a clone or a
  `git archive` export, at any time, have the same SHA-256 (fixed timestamps from the commit time or
  `SOURCE_DATE_EPOCH`, sorted entries, fixed permissions). `packchk`: 0 errors, 0 warnings.

## Tests

- **T6** also counts `osTimerNew()` calls (Arm Compiler `$Sub$$`, GCC `-Wl,--wrap=osTimerNew`): timeout
  guards create no RTOS timer (2.0.0: one per guard, so T6 fails on 2.0.0).
- **T20** accuracy (`d`..`d + 1` ticks), **T21** a timeout racing a channel that becomes ready around the
  deadline (one guard selected; the item neither lost nor doubled), **T22** several timeout guards,
  **T23** zero timeout, **T24** a stale wakeup every tick does not postpone a timeout (2.0.0: 60 ticks
  instead of 10).
- **Positive control, permanent:** FreeRTOS with `configTIMER_TASK_PRIORITY 2`, with Arm's and with ST's
  adapter (MPS2 Cortex-M4 FVP): 2.0.0 ends in a HardFault, 2.0.1 passes.
- **ST's wrapper** (FreeRTOS 10.3.1, STM32CubeG4 1.6.3) is now a regular FVP configuration, including a
  heap-free build.
