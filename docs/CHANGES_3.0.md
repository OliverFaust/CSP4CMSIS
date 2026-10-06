# CSP4CMSIS 3.0.0: changes

The "book API": the API the book prints. 3.0 removes everything deprecated in 2.1.0 and every second
name for the same thing, hides internals that were reachable by accident, and makes the most likely
mistakes compile errors. The channel and ALT protocols are **unchanged**, so the ProB results in
`docs/formal/` still apply, as do the guarantees about memory and interrupts.

## API stability

**3.x will not break source compatibility.** Code that compiles with 3.0 compiles with every 3.x release;
3.x may add API and fix bugs. A change that breaks source compatibility waits for 4.0, with a
deprecation (a compiler warning) in a 3.x release first.

What counts as the API: everything in namespace `csp` outside `csp::internal`, the extern "C" hook
`csp4cmsis_fatal_error`, the configuration defines in `Documentation/CSP4CMSIS_Configuration.md` and the
version macros. `csp::internal` (and `csp::internal::Access`, which the regression suite uses) is not.

## Migrating from 2.x

| 2.x | 3.0 |
|---|---|
| `SleepFor(10)` (ticks) | `SleepFor(Ticks(10))`, or a duration: `SleepFor(Milliseconds(100))`, `SleepFor(Seconds(1))` |
| `SleepFor(osWaitForever)` | `SleepFor(Forever)` |
| `SleepFor(Milliseconds(5).to_ticks())` | `SleepFor(Milliseconds(5))` |
| `Run(InParallel(...))`, `Run(InParallel(...), priority)` (TerminatingNetwork) | `Run(InParallel(...), ExecutionMode::TerminatingNetwork[, priority])` |
| `Run(process[, priority])` (deprecated in 2.1.0) | `Run(InParallel(process), ExecutionMode::StaticNetwork, priority)` |
| `CSP_DEFAULT_TASK_PRIORITY` (deprecated in 2.1.0) | the priority you want |
| `CSP_LEGACY_PARALLEL_PRIORITY` (deprecated in 2.1.0) | `CSP_DEFAULT_NETWORK_PRIORITY` |
| `One2OneChannel<T>`, `Any2OneChannel<T>`, `SamplingChannel<T>` | `Channel<T>` |
| `BufferedOne2OneChannel<T, N, P>`, `BufferedAny2OneChannel<T, N, P>`, `SamplingBufferedChannel<T, N, P>` | `BufferedChannel<T, N, P>` (`P` defaults to `BufferPolicy::Block`) |
| `SignalChannel<>` | `SignalChannel` |
| `Alternative({in.getGuard(v), timeout.internal_guard_ptr})` | `Alternative(in \| v, timeout)` |
| `alt.addBinding(chanin.getGuard(v))` | `alt.addBinding(chanin \| v)` |
| `time.ticks` | `time.to_ticks()` |
| `#include "csp/time.h"` | `#include "csp/csp4cmsis.h"` (the header is now `csp/csp_time.h`) |
| `#ifdef CSP4CMSIS_ISR_WRITER_API` (and the other 2.x feature macros) | 3.x has all those features; to tell 3.x from older versions: `#if __has_include("csp/csp_version.h")`, then `CSP4CMSIS_VERSION` |
| `-DCSP4CMSIS_RTOS2_BACKEND_FREERTOS -DCSP4CMSIS_STATIC_ALLOCATION` | nothing (the default); still accepted |
| no define (dynamic allocation, before 3.0) | `-DCSP4CMSIS_DYNAMIC_ALLOCATION` |

Sharing: any channel may have several writers and several readers, each with its own `writer()`/
`reader()` end, using plain `<<` and `>>` (what `Any2OneChannel` used to suggest). In an ALT, at most one
reader and one writer of a channel may be ALTing; a second one is a fatal error, as in 2.x.

## Configuration

- **Static allocation is the default.** Every RTOS object CSP4CMSIS creates has a static control block
  unless the project defines `CSP4CMSIS_DYNAMIC_ALLOCATION`. Defining both that and
  `CSP4CMSIS_STATIC_ALLOCATION` is an error.
- **Dynamic allocation fixed for FreeRTOS:** with `CSP4CMSIS_DYNAMIC_ALLOCATION` the RTOS now allocates
  each process's stack together with its control block. Before, a process passed its own (static) stack
  with a dynamic control block, which Arm's FreeRTOS adapter rejects: `osThreadNew()` failed, so no
  process started (2.0.x printed an error and continued; 2.1.0 stopped in the fatal-error hook). This was
  2.x's default configuration, unused by every tested and documented setup (all defined
  `CSP4CMSIS_STATIC_ALLOCATION`); RTX5 accepted the mixed form.
- **The backend is detected:** from `RTE_Components.h` in pack builds, otherwise from `FreeRTOS.h` or
  `rtx_os.h` on the include path. If neither or both headers are reachable and nothing else decides, an
  `#error` asks for `CSP4CMSIS_RTOS2_BACKEND_FREERTOS` or `_RTX5`.
- **FreeRTOS without static allocation support** (`configSUPPORT_STATIC_ALLOCATION 0`) stops the build
  in the library source `glue.cpp`, with the CubeMX setting to change.
- **`CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY`:** a `static_assert` rejects 0 and shifted values (0x50,
  0xA0: FreeRTOS's `configMAX_SYSCALL_INTERRUPT_PRIORITY`), which would mask nothing.
- **`CSProcessStatic<N>`:** a `static_assert` requires `N >= CSP_MIN_STACK_WORDS` (64 words); N is in
  words, and a smaller N is almost certainly a byte count.
- A project now needs one define (`CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY`), plus
  `CSP4CMSIS_DEVICE_HEADER` without packs. The STM32CubeIDE guide is updated (two defines).

## Other changes

- `Time` is a class: `Time(ticks)` (explicit), `to_ticks()`, `constexpr`; new `Ticks(n)` and `Forever`.
  `RelTimeoutGuard(Forever)` is the longest timeout (0xFFFFFFFE ticks).
- `CSP_PRIORITY_UNSPECIFIED` and `CSP_STACK_HWM_UNAVAILABLE` are `constexpr` constants (were macros),
  still usable without `csp::`. New: `CSP_MIN_STACK_WORDS`.
- Version macros: `CSP4CMSIS_VERSION_MAJOR`, `_MINOR`, `_PATCH`, `CSP4CMSIS_VERSION` (30000).
- Hidden (were public by accident): `Chanin`/`Chanout::getGuard()`, `Guard::internal_guard_ptr`, the
  `initializer_list` constructors of `Alternative`, `Alternative::addBinding(internal::Guard*)`,
  `CSProcess::prepareTaskCtx()`, `setTaskHandle()`, `stackBuffer()`, `taskBuffer()`, `Time::ticks`.
  `TaskCtx` moved to `csp::internal`.
- `Channel<T>` and `SignalChannel` are not copyable (like the other channels).

## Unchanged

The channel and ALT protocols (one-winner ALT with re-verified wakeups, buffered channels with their
policies, timer-free timeouts), `Chanin`/`Chanout` and their operators, `isrWriter()`/`IsrChanout`/
`putFromISR()`, `Alternative`/`priSelect()`/`fairSelect()`/`RelTimeoutGuard`, `Barrier`, `InParallel`,
`ExecutionMode`, `forEachProcess`, `stackHighWaterMarkWords()`, `csp4cmsis_fatal_error()` and its
contract, and the run-time behaviour of everything that compiles in both 2.1.0 and 3.0.

## Verification

Regression suite `tests/fvp_sse300/bc_tests.cpp` (T0-T31), migrated to the 3.0 API; it detects older
libraries (`__has_include("csp/csp_version.h")`) and still builds against v1.0.0 and 2.x.

Compile checks (`tests/compile_checks/`): a negative probe for every removed name, every hidden
internal, the two new `static_assert`s and every configuration error (both allocation defines, two
backends, an undetectable backend, FreeRTOS without static allocation); positive probes for the
replacements, backend detection (from `RTE_Components.h` and from `FreeRTOS.h`), the absence of FreeRTOS
declarations with `CSP4CMSIS_DYNAMIC_ALLOCATION`, and the version macros.

RESULTS
