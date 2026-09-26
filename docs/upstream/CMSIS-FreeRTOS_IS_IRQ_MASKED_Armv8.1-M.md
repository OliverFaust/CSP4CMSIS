# DRAFT: issue for ARM-software/CMSIS-FreeRTOS (not filed)

**Title:** `IS_IRQ_MASKED()` ignores BASEPRI on Armv8.1-M (Cortex-M55/M85): RTOS2 calls with BASEPRI raised take the thread path and re-enable interrupts

**Checked against:**
- `main` @ `c3e5dc3c4531f02f47ed54cd05df0b3e16465155` (2026-09-01, pack version `11.3.1-dev`), `CMSIS/RTOS2/FreeRTOS/Source/cmsis_os2.c`
- the released pack `ARM::CMSIS-FreeRTOS@11.3.0` (identical code)

No existing issue matches (searched for "8_1M" and "BASEPRI" on 2026-09-26).

---

### Description

`cmsis_os2.c` decides whether an RTOS2 call runs in "IRQ context" with `IRQ_Context()`. That function
treats *masked interrupts* like ISR context, so that e.g. `osEventFlagsSet()`, `osSemaphoreRelease()` and
`osThreadFlagsSet()` use their `…FromISR` implementations, and blocking calls are rejected. The mask test is
architecture-dependent:

```c
#ifndef __ARM_ARCH_8M_MAIN__
  #define __ARM_ARCH_8M_MAIN__    0
#endif
...
#if   ((__ARM_ARCH_7M__      == 1U) || \
       (__ARM_ARCH_7EM__     == 1U) || \
       (__ARM_ARCH_8M_MAIN__ == 1U))
#define IS_IRQ_MASKED()           ((__get_PRIMASK() != 0U) || (__get_BASEPRI() != 0U))
#elif  (__ARM_ARCH_6M__      == 1U)
#define IS_IRQ_MASKED()           (__get_PRIMASK() != 0U)
...
#else
#define IS_IRQ_MASKED()           (__get_PRIMASK() != 0U)
#endif
```

For **Armv8.1-M Mainline** (Cortex-M55, Cortex-M85), compilers define `__ARM_ARCH_8_1M_MAIN__` and **not**
`__ARM_ARCH_8M_MAIN__`. I checked with Arm Compiler 6.24 (`-mcpu=cortex-m55`); GCC and LLVM behave the
same. So `IS_IRQ_MASKED()` falls through to the PRIMASK-only branch, and a raised BASEPRI is not detected,
although these cores have BASEPRI.

CMSIS-RTX 5.9.1 handles this case (`rtx_core_cm.h`, `IsIrqMasked()`):

```c
#if   ((defined(__ARM_ARCH_7M__)        && (__ARM_ARCH_7M__        != 0)) || \
       (defined(__ARM_ARCH_7EM__)       && (__ARM_ARCH_7EM__       != 0)) || \
       (defined(__ARM_ARCH_8M_MAIN__)   && (__ARM_ARCH_8M_MAIN__   != 0)) || \
       (defined(__ARM_ARCH_8_1M_MAIN__) && (__ARM_ARCH_8_1M_MAIN__ != 0)))
  return ((__get_PRIMASK() != 0U) || (__get_BASEPRI() != 0U));
```

### Effect

`IRQ_Context()` is used by 62 functions in `cmsis_os2.c`. On Cortex-M55/M85 with BASEPRI raised (e.g. an
application critical section via `__set_BASEPRI_MAX()`):

1. **The thread path is taken.** For `osEventFlagsSet()` that is `xEventGroupSetBits()`, which runs
   `vTaskSuspendAll()`/`xTaskResumeAll()`. `xTaskResumeAll()` uses `taskENTER_CRITICAL()`/`taskEXIT_CRITICAL()`.
   When the nesting count returns to 0, `vPortExitCritical()` executes `portENABLE_INTERRUPTS()` =
   `vClearInterruptMask(0)`, i.e. `msr basepri, 0`. **The caller's BASEPRI section silently ends inside
   the RTOS call**, and a pended context switch (PendSV) is taken immediately, before the caller restores
   its BASEPRI.
2. Blocking calls made with BASEPRI raised (`osDelay()`, `osSemaphoreAcquire(…, osWaitForever)`, …) are not
   rejected with `osErrorISR`, as they are on Armv7-M / Armv8-M Mainline.

The behaviour of the same source therefore differs between an M33 (ISR path) and an M55 (thread path).

### Reproduction (Cortex-M55, Corstone-300 FVP, Fast Models 11.28.32)

The values below were observed with an equivalent sequence: `__set_BASEPRI_MAX(0xA0)`, then
`osEventFlagsSet()` via a small wrapper, then reading BASEPRI; this is test T2 in the CSP4CMSIS
analysis. The snippet shows the minimal form; it has not been run verbatim.

```c
static osEventFlagsId_t ef;           // created with osEventFlagsNew(NULL)
static volatile int woken = 0;

void high_prio_thread(void *arg) {    // priority above the caller
  osEventFlagsWait(ef, 1U, osFlagsWaitAny, osWaitForever);
  woken = 1;
  for (;;) osDelay(osWaitForever);
}

void caller(void) {                   // runs after the high-priority thread blocked
  __set_BASEPRI_MAX(5U << (8U - __NVIC_PRIO_BITS));   // 0xA0 with 3 priority bits
  uint32_t before = __get_BASEPRI();
  osEventFlagsSet(ef, 1U);
  uint32_t after  = __get_BASEPRI();
  int ran_inside  = woken;
  __set_BASEPRI(0U);
  // Observed: before = 0xA0, after = 0x00, ran_inside = 1
  // Expected (as on Armv7-M / Armv8-M Mainline): after = 0xA0, ran_inside = 0,
  //          osEventFlagsSet() handled via the ISR path.
}
```

In the disassembly of the linked image, `IRQ_Context` reads only `IPSR` and `PRIMASK`.

### Suggested fix

Add Armv8.1-M Mainline to the BASEPRI branch (default the macro like the others):

```c
#ifndef __ARM_ARCH_8_1M_MAIN__
  #define __ARM_ARCH_8_1M_MAIN__  0
#endif
...
#if   ((__ARM_ARCH_7M__        == 1U) || \
       (__ARM_ARCH_7EM__       == 1U) || \
       (__ARM_ARCH_8M_MAIN__   == 1U) || \
       (__ARM_ARCH_8_1M_MAIN__ == 1U))
#define IS_IRQ_MASKED()           ((__get_PRIMASK() != 0U) || (__get_BASEPRI() != 0U))
```

It may be worth checking the other `__ARM_ARCH_*` switches in the repository in the same way.

### Note on the ISR path taken with the fix

`osEventFlagsSet()` from "IRQ context" uses `xEventGroupSetBitsFromISR()`, which defers the operation to
the timer daemon via `xTimerPendFunctionCallFromISR()`. It returns `osErrorResource` if the timer command
queue is full. That is existing, documented behaviour, but callers that set flags with interrupts masked
should check the return value.
