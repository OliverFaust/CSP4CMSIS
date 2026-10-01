# ST's CMSIS-RTOS2 wrapper and CSP4CMSIS

**Compared:** `Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c` of STM32Cube FW_G4 V1.6.3
(FreeRTOS 10.3.1; "Copyright (c) 2013-2020 Arm Limited", modified by ST) against Arm's
`ARM::CMSIS-FreeRTOS@11.3.0` `CMSIS/RTOS2/FreeRTOS/Source/cmsis_os2.c`, which CSP4CMSIS 2.0 is verified
on. Only the functions CSP4CMSIS calls were compared line by line: `osThreadNew`, `osThreadExit`,
`osThreadGetId`, `osThreadGetStackSpace`, `osThreadFlagsSet/Clear/Wait`, `osSemaphoreNew/Acquire/
Release/GetCount/Delete`, `osTimerNew/Start/Stop/Delete`, `osDelay`, `osKernelGetTickFreq`.

**Run-time check:** the regression suite on the MPS2 Cortex-M4 FVP (Armv7E-M) with ST's wrapper and
FreeRTOS 10.3.1 built from the firmware package, `FreeRTOSConfig.h` from the CubeMX project of the guide
(`docs/results_nucleo_g474.md`).

**CSP4CMSIS 2.0.1 uses no CMSIS-RTOS2 timers** (`docs/CHANGES_2.0.1.md`). Rows 1, 2, 3 (timer path)
and 5 below therefore concern 2.0.0 only; row 4 (interrupt-context detection) still applies. With 2.0.1
the suite passes completely on ST's wrapper, including a heap-free build (`docs/results_nucleo_g474.md`).

## Differences that matter to CSP4CMSIS

| # | Area | ST wrapper (FW_G4 1.6.3) | Arm adapter (11.3.0) | Effect on CSP4CMSIS | Evidence |
|---|---|---|---|---|---|
| 1 | `osTimerNew()` static path | Always `pvPortMalloc(sizeof(TimerCallback_t))` (8 B) for the callback record; `cb_mem` must be ≥ `sizeof(StaticTimer_t)` and holds only the timer | Stores the callback record after the `StaticTimer_t` if `cb_size ≥ sizeof(StaticTimer_t) + sizeof(TimerCallback_t)`, else allocates it (only if `configSUPPORT_DYNAMIC_ALLOCATION`) | `csp_rtos_static.h`'s `csp_static_timer_storage_t` (`StaticTimer_t` + 2 pointers) is accepted, the extra 8 B stay unused, and **every `RelTimeoutGuard` allocates 8 B (16 B with `heap_4`'s header) while it exists.** `CSP4CMSIS_STATIC_ALLOCATION` is not heap-free on this wrapper; a heap must be linked. | T6 **FAIL**: "16 B heap while alive; 200 loop constructions -> 200 allocations" (passes on Arm's adapter) |
| 2 | `osTimerDelete()` | Frees the callback record right after `xTimerDelete(h, 0)` returns pdPASS; if the timer command queue is full, returns `osErrorResource` and leaks it. With `USE_FreeRTOS_HEAP_1`: returns `osError`, deletes nothing | Frees the record only if it was allocated | Frees are balanced in the suite (heap delta 0 after T6). A full timer queue would leak 8 B per guard; see 5 | T6 heap delta |
| 3 | Allocator references | `pvPortMalloc` in `osTimerNew()` unconditionally (plus `osThreadEnumerate()`, memory pools) | Same for enumerate and pools; not in the timer path | With ST's wrapper a heap-free build (`configSUPPORT_DYNAMIC_ALLOCATION 0`, no `heap_*.c`) cannot create any timer: `osTimerNew()` returns NULL, which CSP4CMSIS treats as fatal. **T19 / heap-free proof is not possible on this wrapper** (not built) | source |
| 4 | Interrupt-context detection | `IS_IRQ()` = `IPSR != 0` only | `IRQ_Context()`: IPSR ≠ 0, **or** scheduler started and PRIMASK/BASEPRI ≠ 0 | CSP4CMSIS itself calls no CMSIS-RTOS2 function inside its critical sections except `osThreadGetId()` (identical in both, no masking). Test T2 confirms 0 RTOS calls with BASEPRI raised. **For applications:** a thread that calls an RTOS2 function (including a CSP4CMSIS channel operation) with interrupts masked gets the task-level FreeRTOS call; its `taskEXIT_CRITICAL()` then clears BASEPRI and ends the caller's masked section. On Arm's adapter the call takes the FromISR path instead. Do not call into the RTOS from masked thread code | source; T2 PASS |
| 5 | Timer commands are asynchronous (**both adapters**) | `osTimerStart/Stop/Delete` post commands to the FreeRTOS timer service task (queue length 10) | same | CSP4CMSIS's `TimerGuard` assumes the commands are processed before the guard's storage goes out of scope. With `configTIMER_TASK_PRIORITY` below the thread that destroys a `RelTimeoutGuard`, deleted timers are processed after their stack storage has been reused. **CubeMX's default `configTIMER_TASK_PRIORITY 2` breaks CSP4CMSIS; Arm's CMSIS-FreeRTOS template uses 40 (`osPriorityHigh`), the harnesses 55.** Not a wrapper difference: it is a FreeRTOS configuration requirement of CSP4CMSIS 2.0 (RTX5 executes timer calls synchronously) | priority 2: suite HardFault (CFSR 0x400) after T17 with **ST's and Arm's** adapter; the guide example hangs (> 900 s simulated). Priority 55: all pass. Arm's adapter at 40 (= the runner's priority): all pass |

## Checked and equivalent for CSP4CMSIS

| Area | Finding |
|---|---|
| `osThreadNew()` static path | Both: static only if `cb_mem != NULL && cb_size >= sizeof(StaticTask_t) && stack_mem != NULL && stack_size > 0`; then `xTaskCreateStatic()` with `stack_size / sizeof(StackType_t)` words. `csp_static_thread_storage_t = StaticTask_t` matches. Mixed (e.g. `cb_mem` without `stack_mem`) → NULL in both. |
| `osSemaphoreNew()` static path | Both: static if `cb_mem != NULL && cb_size >= sizeof(StaticSemaphore_t)`; binary (`max_count == 1`) → `xSemaphoreCreateBinaryStatic()`, else `xSemaphoreCreateCountingStatic()`. `csp_static_semaphore_storage_t = StaticSemaphore_t` matches. Works before `osKernelInitialize()` (T14). |
| `osEventFlagsNew()` static path | Same check (`StaticEventGroup_t`); CSP4CMSIS 2.0 no longer creates event flags. |
| Thread flags | Both on task notification index 0 (FreeRTOS 10.3.1 has only one). `osThreadFlagsSet` identical except for the context check. |
| `osThreadFlagsClear()` | ST: read (`xTaskNotifyAndQuery`) then overwrite (`eSetValueWithOverwrite`): **not atomic**; a flag set by another thread or an ISR between the two calls is lost. Arm: `ulTaskNotifyValueClear()` (atomic). CSP4CMSIS clears only at the start of a `select()` round, before it registers any guard; nothing may legitimately signal it then, so only stale flags can be lost, which the re-verification ignores anyway. Applications that use their own thread flags on CSP processes (not supported: CSP4CMSIS reserves index 0) could lose flags. |
| `osThreadFlagsWait()` | ST lacks Arm's re-notify: when other flags remain set after a wait returns, Arm re-pends the notification; ST does not, so a following wait for those flags blocks until the next notification. CSP4CMSIS always waits for a flag that is set *after* the state change it waits for (rendezvous `done`, ALT claim), so it never relies on a leftover flag. Also different: timeout recomputation (`tout -= td` vs `timeout - td`); CSP4CMSIS waits with `osWaitForever` only. |
| `osSemaphoreGetCount()` in ISR | `uxQueueMessagesWaitingFromISR` vs `uxSemaphoreGetCountFromISR`: same value. |
| `osTimerStart(t, 0)` | ST passes 0 to FreeRTOS (which asserts); Arm returns `osErrorParameter`. CSP4CMSIS never starts a timer with 0 ticks (`TimerGuard::enable()` handles a zero delay without the timer). |
| `osKernelStart()` | ST additionally resets the SVCall priority to 0. No effect on CSP4CMSIS. |
| `osThreadExit()` | Both `vTaskDelete(NULL)` (ST: not with `USE_FreeRTOS_HEAP_1`). |

## Suite results on ST's wrapper (MPS2 Cortex-M4 FVP, GCC)

| Library | `configTIMER_TASK_PRIORITY` | `-O0` | `-O2` |
|---|---|---|---|
| 2.0 (`76194a5`, library as 2.0.0 + 2.0.1 header) | 55 | PASS=23 **FAIL=1 (T6)** SKIP=0 REPLACED=4; sweeps BUG=0 ANOMALY=0; T13/T13b 0 spins; heap used 152 B | same |
| 2.0 | 2 (CubeMX default) | **HardFault** after T17 (also with Arm's adapter) | not run |
| v1.0.0 (`a789d2a`) | 55 | PASS=8 FAIL=18 SKIP=2: the same 18 failures as with Arm's adapter | not run |

Logs: `tests/fvp_sse300/results/mps2_m4_st_wrapper/`.
