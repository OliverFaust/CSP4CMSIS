# CSP4CMSIS: required project configuration

Every project consuming CSP4CMSIS must set the following in its own
`.cproject.yml` (or equivalent build configuration) -- CSP4CMSIS does not
default any of these, by design: a silently-wrong default is worse than a
build that refuses to compile until you've made a deliberate choice.

## 1. Select your CMSIS-RTOS2 backend (required, no default)

Define exactly one of:
- `CSP4CMSIS_RTOS2_BACKEND_FREERTOS` -- for the CMSIS-RTOS2-over-FreeRTOS
  adapter (`CMSIS:RTOS2:FreeRTOS`).
- `CSP4CMSIS_RTOS2_BACKEND_RTX5` -- for native RTX5
  (`CMSIS:RTOS2:Keil RTX5`).

This selects the correct static-allocation control-block types in
`csp_rtos_static.h` (only relevant if you also enable
`CSP4CMSIS_STATIC_ALLOCATION`, below) and the correct `FreeRTOSConfig.h`-
style bootstrap code path where one is needed. Leaving this undefined is a
hard compile error (`#error`) by design -- CSP4CMSIS will not guess.

## 2. Static allocation (optional)

Define `CSP4CMSIS_STATIC_ALLOCATION` to back CSP4CMSIS's RTOS2 objects
(thread TCBs, event flags) with static, no-heap storage instead of dynamic
allocation. Requires (1) above to also be set, so the correct backend-
specific control-block types can be resolved.

If you leave this undefined, CSP4CMSIS uses dynamic (heap) allocation for
these objects -- genuinely portable across any CMSIS-RTOS2 backend, and
the safe default if you haven't verified your backend's static-allocation
control-block types yourself.

**This only governs CSP4CMSIS's own internal RTOS2 objects.** It has
nothing to do with whether your own application code allocates -- see the
note on `operator new`/`operator delete` at the end of this document.

## 3. `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` (always required)

This is the one most worth reading carefully, because the obvious
assumption about what it depends on is wrong.

**It is not primarily about which RTOS backend you're using.** It's easy
to assume this value needs to match your RTOS's own interrupt-masking
threshold (e.g. FreeRTOS's `configMAX_SYSCALL_INTERRUPT_PRIORITY`) -- and
historically, matching that value was in fact the correct answer on the
FreeRTOS-backed adapter. But when CSP4CMSIS was validated against native
RTX5, the same numeric value carried over correctly even though **RTX5
has no equivalent setting at all** -- RTX5's own critical sections use
`LDREX`/`STREX` exclusive-access atomics, not priority-threshold masking,
so there's nothing RTOS-side to "match" on that backend.

**What it actually depends on: your board's peripheral interrupt
priorities.** CSP4CMSIS's `BufferedChannel` and `putFromISR()` use a
portable `BASEPRI`-based critical section (`csp_critical.h`) to protect
their internal state. For that critical section to behave correctly, its
threshold needs to sit:
- **Below** the priority of any interrupt that might call into CSP4CMSIS
  (e.g. a `putFromISR()` call from your own ISR) -- so that interrupt gets
  correctly masked while a critical section is active.
- **Above** the priority of any interrupt that must never be blocked, even
  during a CSP4CMSIS critical section (commonly: a console/debug UART, a
  time-critical sensor interface, an inter-processor doorbell -- whatever
  your board's `RTE_Device.h`/NVIC configuration sets at a numerically low
  (high-urgency) priority value).

**How to derive the correct value for your project:**
1. List every interrupt priority your project explicitly sets (grep for
   `NVIC_SetPriority` calls and your device pack's `RTE_Device.h`-style
   generated priority macros).
2. Identify any that must remain unmasked during a CSP4CMSIS critical
   section (typically console/debug output and any hard-real-time
   peripheral interfaces).
3. Set `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` numerically *above* the
   highest-urgency (numerically lowest) priority found in step 2, and
   *below or equal to* the priority of anything that's safe to defer
   during a critical section (remember: lower number = higher urgency on
   Cortex-M).
4. Confirm on real hardware -- flash and verify nothing that should stay
   responsive during a critical section stalls unexpectedly.

**This define is required in every build**, whatever channels you use.
Every channel kind and `Alternative::select()` protect their state with this
critical section: buffered channels, rendezvous and signal channels (whose
ALT state words and registrations are updated inside it), and the ALT
scheduler itself. The library's own sources (`alternative.cpp`,
`alt_channel_sync.cpp`) and `buffered_channel.h` (included by
`csp4cmsis.h`) include `csp_critical.h`, which stops the build with
`#error` if the define is missing. There is no default on purpose: a wrong
value either masks interrupts that must never be delayed, or leaves an ISR
that writes into a buffered channel unmasked.

Pass the **unshifted** priority number (e.g. `5` on a device with 3
priority bits), not the `BASEPRI` register value: `csp_critical.h` shifts it
by `8 - __NVIC_PRIO_BITS` itself. Passing an already-shifted value such as
`0xA0` makes `BASEPRI` 0 after the shift and turns every critical section
into a no-op, silently.

## 4. `CSP4CMSIS_ISR_MAX_ELEMENT_SIZE` (optional, default 64)

An ISR writes into a buffered channel through its ISR writer end
(`auto isr_out = chan.isrWriter();`, then `isr_out.putFromISR(v)`). The
element is copied into the channel with `BASEPRI` raised, so every interrupt
at or below `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` waits for that copy.
To keep this bounded, `IsrChanout<T>` contains
`static_assert(sizeof(T) <= CSP4CMSIS_ISR_MAX_ELEMENT_SIZE)`. Rendezvous
and signal channels have no ISR writer.
The default of 64 bytes (a 16-word copy) costs about as much as an RTOS
queue operation.

- Only code that takes an ISR writer end (`isrWriter()`) is checked; channels
  of large types used only between tasks are unaffected.
- To allow larger ISR writes, define the limit for the whole project, e.g.
  `-DCSP4CMSIS_ISR_MAX_ELEMENT_SIZE=256`, after checking the extra
  interrupt latency on your target.
- Better for large payloads: keep them in a statically allocated pool and
  send the index (`uint8_t`) through the channel; see the "Masked copy"
  comment in `buffered_channel.h`.

## 5. Where channels may be constructed

Every channel creates its RTOS objects (semaphores, mutexes) **in its
constructor**, and a failed creation is fatal (`csp4cmsis_fatal_error()`).
So the kernel must accept object creation when the constructor runs.

**Allowed:**
- **Namespace scope** (global or `static` at file scope), i.e. during C++
  static initialisation before `main()`;
- **function-local `static`**, constructed on first use;
- automatic (stack) objects in a process, provided they outlive every use by
  other processes.

**Why namespace scope works on both backends** (measured by test T14 on the
Corstone-300 FVP, Arm Compiler 6 and GCC):
- **FreeRTOS** (CMSIS-FreeRTOS adapter): before `main()` the kernel is
  `osKernelInactive`, and static and dynamic `osSemaphoreNew()` both succeed.
- **Keil RTX5** (CMSIS-RTX 5.9.1): the kernel is already `osKernelReady`
  before C++ constructors run, because CMSIS-RTX calls
  `osKernelInitialize()` from a C-library start-up hook:
  `_platform_post_stackheap_init()` (Arm Compiler), `software_init_hook()`
  (GCC/newlib) or `$Sub$$__iar_data_init3` (IAR). A later explicit
  `osKernelInitialize()` in `main()` is harmless.

**Rules:**
- On RTX5, do not override `_platform_post_stackheap_init()`,
  `software_init_hook()` or `__iar_data_init3` unless your version also
  calls `osKernelInitialize()` first; and do not bypass the C library's
  start-up. Otherwise namespace-scope channels fail at boot (loudly, via
  `csp4cmsis_fatal_error()`).
- Other CMSIS-RTOS2 backends: namespace-scope construction needs object
  creation to work before `main()`. If your backend cannot do that, use
  function-local statics constructed after `osKernelInitialize()`.
- **Never construct a channel in an ISR**, and never let a channel be
  destroyed while any process or ISR may still use it.
- A channel must be constructed before any ISR write to it can run:
  enable the interrupt only after the channel exists.

## Application-level dynamic allocation (not CSP4CMSIS's concern)

CSP4CMSIS itself never calls `operator new`/`operator delete` and performs
no dynamic allocation of its own -- it's designed to be usable in a
zero-heap system. Whether *your application code* allocates (e.g. an
inference pipeline using `std::vector`) is entirely your own architectural
decision, and if you need a working global `operator new`/`operator
delete`, that's your project's responsibility to provide -- not
CSP4CMSIS's. If you do provide one, be aware that some newlib-nano/
toolchain/port combinations do not wire up thread-safe malloc locking by
default; verify yours does before relying on a plain unguarded `malloc`
from multiple threads.
