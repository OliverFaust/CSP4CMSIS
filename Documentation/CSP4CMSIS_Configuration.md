# CSP4CMSIS: required project configuration

Every project consuming CSP4CMSIS must set the following in its own
`.cproject.yml` (or equivalent build configuration) -- CSP4CMSIS does not
default any of these, by design: a silently-wrong default is worse than a
build that refuses to compile until you've made a deliberate choice.

## 1. Select your CMSIS-RTOS2 backend (required with `CSP4CMSIS_STATIC_ALLOCATION`)

Define exactly one of:
- `CSP4CMSIS_RTOS2_BACKEND_FREERTOS` -- for any CMSIS-RTOS2 layer over FreeRTOS: Arm's adapter
  (`CMSIS:RTOS2:FreeRTOS`) or ST's STM32Cube wrapper (CubeMX "CMSIS_V2").
- `CSP4CMSIS_RTOS2_BACKEND_RTX5` -- for native RTX5 (`CMSIS:RTOS2:Keil RTX5`).

Its only effect: it selects the backend's static control-block types in `csp_rtos_static.h`
(`StaticTask_t`/`StaticSemaphore_t` or RTX5's `osRtx*_t`), which `CSP4CMSIS_STATIC_ALLOCATION`
(section 2) needs. With `CSP4CMSIS_STATIC_ALLOCATION` and neither define, the build stops with an
`#error` -- CSP4CMSIS will not guess. Without `CSP4CMSIS_STATIC_ALLOCATION` the define is not used;
defining it anyway does no harm and keeps the configuration ready for static allocation.

## 2. Static allocation (optional, required for a heap-free system)

Define `CSP4CMSIS_STATIC_ALLOCATION` to give **every** RTOS2 object that
CSP4CMSIS creates a statically allocated control block. That covers:
- process threads (`CSProcessStatic<N>`, `Run()`);
- `Run()`'s completion semaphore;
- the semaphores of buffered, rendezvous and signal channels and of `Barrier`.

Timeout guards (`RelTimeoutGuard`) create no RTOS object at all (2.0.1, section 8).

Stacks are always static (`CSProcessStatic<N>`). The macro requires (1) above, so that the correct
backend-specific control-block types are used.

If you leave it undefined, those control blocks come from the RTOS's own
allocator (FreeRTOS heap, RTX5 dynamic memory). That is portable across any
CMSIS-RTOS2 backend, and the safe default if you haven't verified your
backend's static control-block types yourself.

**This only governs CSP4CMSIS's own RTOS2 objects.** What else a system
needs to be completely heap-free is in section 6.

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
**Measured on hardware** (Alif DK-E8, Cortex-M55 at 400 MHz, code in ITCM,
RTX5, Arm Compiler 6.24; `docs/hardware_results_dk_e8.md`). An interrupt
*above* `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` is not delayed at all. One
*below* it can be delayed by at most the masked copy:

| Element size | `-O0`: `putFromISR()` / worst added delay | `-O2`: `putFromISR()` / worst added delay |
|---|---|---|
| 4 B | 158 cycles / 64 cycles (0.16 µs) | 43 cycles / 24 cycles (0.06 µs) |
| 64 B (default limit) | 211 cycles / 117 cycles (0.29 µs) | 97 cycles / 77 cycles (0.19 µs) |
| 1024 B (limit raised) | 511 cycles / 417 cycles (1.04 µs) | 368 cycles / 212 cycles (0.53 µs) |

`putFromISR()` is measured for a `KeepNewest` write into a full channel (copy
only, no RTOS call). The added delay is the latency of a probe interrupt
triggered at random points during back-to-back copies, minus the latency with
plain copies. For comparison, the RTOS part of a `putFromISR()` that wakes a
waiting reader takes about 750 cycles at `-O0` and 220 at `-O2`. So the
64-byte default adds less delay than one RTOS operation. Other cores and
clocks scale roughly with the copy loop (bytes per cycle).

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

## 6. Heap-free systems: what CSP4CMSIS guarantees, and what else you need

### What CSP4CMSIS guarantees

- **The library performs no dynamic memory allocation.** Its code never calls `malloc`/`free`,
  `operator new`, `pvPortMalloc()` or any other allocator. (The compiler references sized
  `operator delete` from the deleting destructors of classes with virtual destructors; CSP4CMSIS never
  `delete`s anything, so it is never called.)
- **With `CSP4CMSIS_STATIC_ALLOCATION`, it makes no dynamic RTOS allocation either:** every RTOS2
  object it creates has a static control block (section 2). Channels, guards, `Alternative`s and
  processes are ordinary objects that you place (statically or on a stack).
- **How this is verified** (`tests/fvp_sse300/`, "Heap-free proof"): the full regression suite passes on
  both backends with RTOS dynamic allocation disabled, with Arm Compiler 6 and GCC. The CSP4CMSIS object
  files reference no allocation function, and test T17 constructs 50 × (rendezvous channel + signal
  channel + `Barrier`) without a single RTOS allocation.

**Not guaranteed by CSP4CMSIS** (your system's responsibility): the RTOS's own configuration, objects
that your application creates, and the C library (below).

**ST's STM32Cube CMSIS-RTOS2 wrapper** (FreeRTOS 10.3.1 in STM32Cube FW_G4 1.6.3) gives the same
guarantees from 2.0.1 on: the suite passes on it with RTOS dynamic allocation disabled (MPS2 Cortex-M4
FVP, `FreeRTOS-ST-NoHeap`). Its `cmsis_os2.c` references `pvPortMalloc()`/`vPortFree()` unconditionally
(timers, thread enumeration, memory pools), so a build without a heap implementation needs the traps of
workaround B below. (In 2.0.0 every live `RelTimeoutGuard` held 16 bytes of FreeRTOS heap on this
wrapper, because its `osTimerNew()` always allocates; 2.0.1 creates no timer.) Details:
`docs/st_cmsis_rtos2_wrapper.md`.

### What a fully heap-free system additionally needs

**Common to both backends:**
- Define `CSP4CMSIS_STATIC_ALLOCATION` (and the backend define it requires).
- Create every application thread and RTOS object with static memory (`cb_mem`/`cb_size`, and
  `stack_mem`/`stack_size` for threads).
- Verify the result in the map file: no allocator symbol (`pvPortMalloc` / RTX5 `os_mem`) where there
  should be none.

**FreeRTOS (CMSIS-FreeRTOS 11.3.0 adapter):**
1. `FreeRTOSConfig.h`: `configSUPPORT_STATIC_ALLOCATION 1`, `configSUPPORT_DYNAMIC_ALLOCATION 0`,
   `configKERNEL_PROVIDED_STATIC_MEMORY 1` (or provide `vApplicationGetIdleTaskMemory()` and
   `vApplicationGetTimerTaskMemory()` yourself).
2. Remove the Heap component (`ARM::RTOS&FreeRTOS:Heap&Heap_*`): `heap_*.c` stops the build with
   `#error` when dynamic allocation is 0. csolution then reports a failed dependency validation
   ("FreeRTOS Heap") as a warning; that is expected.
3. **Workaround A: C library locks (Arm Compiler only).** The adapter's `clib_os.c` falls back to the
   dynamic `xSemaphoreCreateMutex()` without checking `configSUPPORT_DYNAMIC_ALLOCATION`, so it does not
   compile. Force-include a header in every compilation unit (csolution:
   `misc: - C-CPP: [-include <path>/noheap_shim.h]`) containing
   ```c
   #define xSemaphoreCreateMutex() ((void *)0)
   ```
   The C library's mutexes then come only from the adapter's static pool (`OS_MUTEX_CLIB_NUM`, default
   5). If the pool is too small, `_mutex_initialize()` fails and that C-library stream is unlocked;
   raise `OS_MUTEX_CLIB_NUM` if you use many streams.
4. **Workaround B: allocator references.** The adapter's `cmsis_os2.c` references `pvPortMalloc()` and
   `vPortFree()` unconditionally (`osThreadEnumerate()`, `osMemoryPoolNew()`/`osMemoryPoolDelete()`).
   With no heap implementation, Arm Compiler fails to link (`L6218E: Undefined symbol pvPortMalloc`).
   Provide trap definitions that report a call and return NULL:
   ```c
   void *pvPortMalloc(size_t n) { (void)n; /* report */ return NULL; }
   void vPortFree(void *p)      { (void)p; /* report */ }
   ```
   If nothing calls these functions, the linker removes the traps (Arm Compiler); with GCC they may
   remain linked. Either way, a call at run time is reported instead of allocating.
   `configUSE_OS2_THREAD_ENUMERATE 0` removes one of the two users.

**Keil RTX5 (CMSIS-RTX 5.9.1):**
1. `RTX_Config.h`: `OS_DYNAMIC_MEM_SIZE 0`. This removes the dynamic memory pool; any object created
   without `cb_mem` then fails (CSP4CMSIS treats that as fatal). Object-specific pools
   (`OS_*_OBJ_MEM`) are fixed, statically allocated arrays and may still be used.
2. **Workaround C: C library locks (Arm Compiler only).** RTX5 creates the Arm C library's stream
   mutexes with `osMutexNew(NULL)` (`rtx_lib.c`, `_mutex_initialize()`). Without dynamic memory they need
   the static mutex pool: `OS_MUTEX_OBJ_MEM 1`, `OS_MUTEX_NUM 8` (the FVP harness uses 8). Without it
   start-up stops in `osRtxErrorNotify(osRtxErrorClibMutex, …)` before `main()`.
3. The idle and timer threads are static by default (`OS_IDLE_THREAD_*`, `OS_TIMER_THREAD_*`).

The two CMSIS-FreeRTOS defects are reported upstream as drafts in `docs/upstream/` (not yet filed).

### The C library heap: an application concern

The C library has its own heap (`malloc`), independent of the RTOS:
- **`printf` and other stdio functions may allocate.** Both the Arm C library and newlib are linked with
  `malloc` in the FVP harness images, although CSP4CMSIS never calls it.
- If your system must not have a C heap either: avoid stdio, or retarget output without it. Making a
  stream unbuffered (`setvbuf(stdout, NULL, _IONBF, 0)` before first use) removes the stream buffer,
  but not every allocation: newlib's floating-point `printf` formatting, for example, allocates through
  its `dtoa` helpers. Check the map file for `malloc`, and size the heap region (`ARM_LIB_HEAP` /
  newlib's heap between `_end` and the heap limit) to zero, so that any remaining use becomes a link or
  run-time failure. (Not verified in the CSP4CMSIS test harness, which uses `printf`.)
- Whether *your application code* allocates (e.g. an inference pipeline using `std::vector`) is your own
  decision. If you provide a global `operator new`/`delete`, make sure your C library's `malloc` locking
  is thread-safe with your RTOS (some newlib-nano/toolchain/port combinations do not wire it up by
  default).

## 7. Builds without CMSIS packs: `CSP4CMSIS_DEVICE_HEADER`

`csp_critical.h` needs the device's CMSIS device header (`__NVIC_PRIO_BITS`, the `BASEPRI` intrinsics).
Pack builds (CMSIS-Toolbox, µVision) get it from the generated `RTE_Components.h`
(`CMSIS_device_header`); nothing to set. Builds without packs (STM32CubeIDE, vendor SDK makefiles) have
no `RTE_Components.h` and must name the header:

```
-DCSP4CMSIS_DEVICE_HEADER="stm32g4xx.h"        (STM32CubeIDE: typed exactly like this)
-DCSP4CMSIS_DEVICE_HEADER=\"WE2_device.h\"     (makefile, Himax WE2)
```

With neither, the build stops with an `#error` that names the define. (2.0.1; CSP4CMSIS 2.0.0 includes
`RTE_Components.h` unconditionally.)

Also for builds without packs:
- **C++17** (`-std=gnu++17` or `-std=c++17`); STM32CubeIDE's default is GNU++14.
- **Include path: `csp4cmsis/inc` only.** Applications include `"csp/csp4cmsis.h"`. Do not add
  `csp4cmsis/inc/csp`: with it on the search path, `#include <time.h>` finds CSP4CMSIS's `time.h`.
- Sources: `csp4cmsis/src/*.cpp`.

Step-by-step for STM32CubeMX/STM32CubeIDE: `Documentation/CSP4CMSIS_STM32CubeIDE.md`.

## 8. ALT timeouts: no RTOS timer, no timer-service requirement (2.0.1)

A `RelTimeoutGuard` is plain data. `select()` fixes a deadline when it starts (the CMSIS-RTOS2 tick count,
`osKernelGetTickCount()`) and waits for its thread flags at most until the earliest deadline of its
timeout guards; a new round (e.g. after a stale wakeup) never postpones it. So:
- no `osTimer`, no RTOS timer service involvement: `configUSE_TIMERS`, the timer service task and its
  priority (FreeRTOS `configTIMER_TASK_PRIORITY`, RTX5 `OS_TIMER_THREAD_PRIO`) do not matter to
  CSP4CMSIS;
- no RTOS memory, static or dynamic, on any backend;
- a timeout of `d` ticks is selected `d` (at most `d + 1`) ticks after `select()` started, unless a guard
  listed before it is ready; a zero timeout is selected at once, without waiting;
- durations are 32-bit tick counts (up to 2^32 - 2 ticks); the tick count may wrap during a `select()`.

**CSP4CMSIS 2.0.0** used one CMSIS-RTOS2 timer per guard; with FreeRTOS that requires the timer service
task to run above every thread that uses timeouts (`docs/known-issues.md`).
