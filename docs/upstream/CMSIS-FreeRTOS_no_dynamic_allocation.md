# Draft issue for ARM-software/CMSIS-FreeRTOS (NOT filed)

**Title:** CMSIS-RTOS2 adapter does not build/link with `configSUPPORT_DYNAMIC_ALLOCATION = 0`

**Version:** ARM::CMSIS-FreeRTOS 11.3.0 (pack), FreeRTOS kernel 11.3.0; Arm Compiler 6.24 and GCC 14.2.1;
Cortex-M55 (Corstone-300 FVP).

**Setup:** `configSUPPORT_STATIC_ALLOCATION = 1`, `configSUPPORT_DYNAMIC_ALLOCATION = 0`,
`configKERNEL_PROVIDED_STATIC_MEMORY = 1`. The application creates every RTOS2 object with `cb_mem`/
`cb_size` (and `stack_mem`). No heap implementation is included, because `heap_*.c` `#error` when
dynamic allocation is 0; the pack's "FreeRTOS Heap" dependency then reports a validation warning.

## 1. `clib_os.c`: dynamic mutex fallback is not guarded (Arm Compiler only)

`CMSIS/RTOS2/FreeRTOS/Source/clib_os.c`, `_mutex_initialize()`:

```c
  if ((*m == NULL) && (os_kernel_is_active())) {
    /* Create mutex using dynamic memory */
    *m = xSemaphoreCreateMutex();
  }
```

`xSemaphoreCreateMutex()` is not declared when `configSUPPORT_DYNAMIC_ALLOCATION == 0`, so the file
fails to compile (`call to undeclared function 'xSemaphoreCreateMutex'`). The static pool
(`OS_MUTEX_CLIB_NUM`) above it would be sufficient.

**Suggested fix:** wrap the fallback in `#if (configSUPPORT_DYNAMIC_ALLOCATION == 1)`.

## 2. `cmsis_os2.c`: `pvPortMalloc()`/`vPortFree()` referenced unconditionally

`osThreadEnumerate()` (`pvPortMalloc(count * sizeof(TaskStatus_t))`, `vPortFree(task)`) and the memory
pool functions (`osMemoryPoolNew()`: `pvPortMalloc(sizeof(MemPool_t))`; `osMemoryPoolDelete()`:
`vPortFree()`) use the heap without checking `configSUPPORT_DYNAMIC_ALLOCATION`.
- Without a heap implementation, armlink reports `L6218E: Undefined symbol pvPortMalloc (referred from
  cmsis_os2.o)` (also `vPortFree`), even if the application never calls these functions.
- With GCC the linked image contains no `pvPortMalloc` when these functions are unused (section garbage
  collection). I did not test whether GCC links without any definition of the symbols.

**Suggested fix:** under `configSUPPORT_DYNAMIC_ALLOCATION == 0`:
- `osThreadEnumerate()` returns 0 (or uses a caller-provided array);
- `osMemoryPoolNew()` accepts only `mp_attr->cb_mem`/`mp_mem` (it already has a static path) and returns
  NULL otherwise;
- `osMemoryPoolDelete()` frees nothing.

## Workaround used (CSP4CMSIS FVP test harness)

- A forced include defining `xSemaphoreCreateMutex()` as `((void *)0)`, so `clib_os.c` uses only its
  static pool.
- Trap definitions of `pvPortMalloc()`/`vPortFree()` that count calls and return NULL. With Arm
  Compiler the linker removes them (no reference remains in the image); with GCC they are linked and
  are never called during the test suite.

## For comparison: RTX5

Keil RTX5 5.9.1 builds with `OS_DYNAMIC_MEM_SIZE = 0` without changes. With Arm Compiler, the C
library's stream mutexes (`rtx_lib.c`, `osMutexNew(NULL)`) then need the object-specific mutex pool
(`OS_MUTEX_OBJ_MEM = 1`, `OS_MUTEX_NUM >= 5`). Otherwise start-up stops in the kernel error handler
before `main()`. This is documented RTX5 behaviour, not a defect.
