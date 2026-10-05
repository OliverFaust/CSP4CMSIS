# CSP4CMSIS 2.1.0: changes

Minor release. Code that builds with 2.0.1 builds with 2.1.0, with deprecation warnings where it uses the
names below, except for an override of `CSProcess::endProcess()` declared `override` (removed, see the
last section). Three behaviour changes turn silent misbehaviour into a fatal error or a longer, correct
wait. Evidence: section "Verification".

## Behaviour changes

| What | 2.0.1 | 2.1.0 |
|---|---|---|
| `osThreadNew()` fails in `Run()` (out of RTOS memory, invalid attributes) | prints `FATAL ERROR: …` and continues: a `TerminatingNetwork` caller waits for ever, and in a `StaticNetwork` the missing process's partners block | `csp4cmsis_fatal_error("CSP4CMSIS: Run(): osThreadNew() failed")`, in both execution modes and in the deprecated single-process `Run()` |
| 17th guard in an `Alternative` (at most 16) | ignored silently; the guard is never selected | `csp4cmsis_fatal_error("CSP4CMSIS: Alternative: more than 16 guards")` when the guard is added (constructor or `addBinding()`) |
| `Seconds(s)`, `Milliseconds(ms)` | rounded down: `Milliseconds(5)` at 100 Hz was `Time(0)`, no wait at all; `ms * freq` overflowed 32 bits above ~71.6 min at 1 kHz | rounded **up**, computed in 64 bits: a non-zero duration is at least 1 tick and never shorter than requested (at most one tick longer); `0` stays `0`; saturated at `0xFFFFFFFE` ticks (`0xFFFFFFFF` is `osWaitForever`) |

`run.h` and `public_task.h` no longer print (`printf`/`<cstdio>` removed from both).

If your application overrides `csp4cmsis_fatal_error()`, the two new messages reach it. The default
records the message in `csp4cmsis_last_fatal_error` and halts.

## Deprecated (warning on use; removed in 3.0)

| Deprecated | Replacement |
|---|---|
| `Run(CSProcess& p, osPriority_t priority = CSP_DEFAULT_TASK_PRIORITY)` | `Run(InParallel(p), ExecutionMode::StaticNetwork, priority)`. Note the defaults differ: the deprecated overload ran the process at `osPriorityRealtime7`; the replacement uses `CSP_DEFAULT_NETWORK_PRIORITY` (`osPriorityLow`) unless you pass a priority or the process overrides `taskPriority()`. The replacement also names the thread after `name()`. |
| `CSP_DEFAULT_TASK_PRIORITY` | none (it only served the overload above); pass the priority you want |
| `CSP_LEGACY_PARALLEL_PRIORITY` | `CSP_DEFAULT_NETWORK_PRIORITY` (same value, `osPriorityLow`) |
| `One2OneChannel<T, P>` | `Channel<T>` (or `SamplingChannel<T, P>`, which is what the alias named) |
| `BufferedOne2OneChannel<T, SIZE, P>` | `BufferedChannel<T, SIZE>`, or `SamplingBufferedChannel<T, SIZE, P>` for `KeepNewest`/`KeepOldest` |

The warnings come from `[[deprecated]]` and appear only where a name is used; including the headers
does not warn.

## Added

- `SleepFor(Time duration)`, e.g. `SleepFor(Milliseconds(250))`; same as `SleepFor(duration.to_ticks())`.
  `SleepFor(uint32_t ticks)` is unchanged; a plain number still means ticks (`Time`'s constructor is
  `explicit`). Feature-test macro: `CSP4CMSIS_SLEEPFOR_TIME_API`.
- `CSP_DEFAULT_NETWORK_PRIORITY` (`osPriorityLow`): the default priority of `Run(InParallel(…), …)`.

## Documentation

- `csp_fatal.h`: the hook may be called from an interrupt handler (an application's ISR may call it, e.g.
  when an `isrWriter()` write fails), so a replacement must be ISR-safe: no stdio, no blocking RTOS
  calls. CSP4CMSIS itself calls it from threads only, never inside a CSP critical section. (2.0.1 said
  an override "may use the RTOS and stdio".)
- `Documentation/CSP4CMSIS_Configuration.md` and `README.md`: `CSP4CMSIS_RTOS2_BACKEND_FREERTOS`/`_RTX5`
  is required only with `CSP4CMSIS_STATIC_ALLOCATION` (it selects the static control-block types); it
  covers both Arm's adapter and ST's STM32Cube wrapper. (2.0.1 called it required in every build.)

## Removed

- `CSProcess::endProcess()`: a protected virtual that nothing ever called. An override declared
  `override` no longer compiles; delete it (its body never ran). An override without `override` compiles
  and, as before, is never called.
- `csp4cmsis/inc/csp/exceptions.h`, `csp4cmsis/inc/csp/test_types.h` and `csp4cmsis/src/kernel.cpp`:
  unused (included or called nowhere; kernel.cpp's code was commented out). The pack never shipped them;
  only a copy of the source tree had them.

## Verification

Regression suite `tests/fvp_sse300/bc_tests.cpp`, new tests T25–T31 (before 2.1.0 the suite never ran a
`CSProcess`, `Run` or `InParallel`):

| Test | Checks |
|---|---|
| T25 | `TerminatingNetwork`: `Run()` returns after every process has returned; a process's `taskPriority()` overrides the composition priority, the others run at it; default composition priority `osPriorityLow` |
| T26 | `StaticNetwork`: `Run()` returns before any lower-priority process has run; threads named after `name()`; all run afterwards |
| T27 | `forEachProcess` visits every process in declaration order; `stackHighWaterMarkWords()` is `CSP_STACK_HWM_UNAVAILABLE` before `Run()`, then in words, reflecting the stack used |
| T28 | forced `osThreadNew()` failure (`$Sub$$` with Arm Compiler, `--wrap=osThreadNew` with GCC) is a fatal error in both execution modes; `Run()` does not return |
| T29 | a 17th guard is a fatal error |
| T30 | `Seconds()`/`Milliseconds()` as properties: 0 stays 0, never shorter, at most one tick longer, no 32-bit overflow, saturation |
| T31 | `SleepFor(Time)` waits the given ticks |

On 2.0.1, T28–T30 fail (the positive control below). Compile checks (`tests/compile_checks/`):
`EXPECT-WARNING` probes for the five deprecations (`dep_*.cpp`), `neg_end_process.cpp`, and
`pos_replacements.cpp` (the replacements compile without a warning).

Results (2026-10-05, `release-2.1.0` @ `c66a8b8`):
- FVP, 30 configurations (Corstone-300 Cortex-M55 and MPS2 Cortex-M4; Arm Compiler 6.24 and GCC 14.2.1;
  FreeRTOS 11.3.0 through Arm's adapter, Keil RTX5, FreeRTOS 10.3.1 through ST's STM32Cube wrapper;
  `-O0`/`-O2`/`-Os`; heap-free builds): PASS=36 FAIL=0 SKIP=0 REPLACED=4 in all 30 (PASS=37 with T19 in the
  4 heap-free builds). `tests/fvp_sse300/results/v2.1.0/`.
- NUCLEO-G474RE, ST's wrapper, Debug `-O0` and Release `-Os`, with and without heap: PASS=36 / 37, FAIL=0.
  `tests/hw_nucleo_g474/results/2026-10-05_2.1.0_*`.
- Positive control, 2.0.1 with the new suite: T28, T29, T30 FAIL, T31 SKIP (`results/v2.1.0/controls_2.0.1/`).
- Compile checks: all 24 probes pass with AC6 and GCC on FreeRTOS, RTX5 and ST's wrapper
  (`results/v2.1.0/compile_checks.txt`).

Not done (release steps): the version number (pdsc `Cversion`, release entry), the API reference page
(`CSP4CMSIS/api.md`: its synopsis still shows the 2.0.1 default arguments, so `tests/doc_examples`
reports three changed synopsis lines until the page is updated), the pack.
