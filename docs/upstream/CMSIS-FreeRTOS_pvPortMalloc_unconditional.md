# Issue for ARM-software/CMSIS-FreeRTOS: ready to file (NOT filed)

**Title:** `cmsis_os2.c` references `pvPortMalloc()`/`vPortFree()` regardless of `configSUPPORT_DYNAMIC_ALLOCATION`: link error without a heap implementation

**Checked against:**
- `main` @ `c3e5dc3c4531f02f47ed54cd05df0b3e16465155` (head on 2026-09-27);
- the released pack `ARM::CMSIS-FreeRTOS@11.3.0`.

The file `CMSIS/RTOS2/FreeRTOS/Source/cmsis_os2.c` is identical in both.

### Description

With `configSUPPORT_DYNAMIC_ALLOCATION = 0`, no `heap_*.c` can be part of the build: they all `#error`
("This file must not be used if configSUPPORT_DYNAMIC_ALLOCATION is 0"). But `cmsis_os2.c` still
calls the heap in three functions, without checking the setting:

| Function | Line (main) | Use |
|---|---|---|
| `osThreadEnumerate()` (under `configUSE_OS2_THREAD_ENUMERATE`) | 911, 925 | `pvPortMalloc(count * sizeof(TaskStatus_t))`, `vPortFree(task)` |
| `osMemoryPoolNew()` | 2529, 2551, 2585 | `pvPortMalloc(sizeof(MemPool_t))`, `pvPortMalloc(sz)`, `vPortFree(mp)` |
| `osMemoryPoolDelete()` | 2892, 2896 | `vPortFree(mp->mem_arr)`, `vPortFree(mp)` |

(The timer callback wrapper at lines 1258–1457 is correctly guarded.)

With Arm Compiler 6.24 the link fails even if the application never calls these functions:

```
Error: L6218E: Undefined symbol pvPortMalloc (referred from cmsis_os2.o).
Error: L6218E: Undefined symbol vPortFree (referred from cmsis_os2.o).
```

With GCC 14.2.1 the linked image contains no `pvPortMalloc` when the functions are unused (section
garbage collection). I did not test whether GCC links without any definition of the symbols.

### Suggested fix

When `configSUPPORT_DYNAMIC_ALLOCATION == 0`:
- `osThreadEnumerate()` returns 0, or is compiled only when dynamic allocation is available;
- `osMemoryPoolNew()` accepts only caller-provided `cb_mem` and `mp_mem` (its static path already
  exists) and returns NULL otherwise;
- `osMemoryPoolDelete()` frees nothing (`status` bits 0 and 1 are never set).

For example, guard each `pvPortMalloc()` / `vPortFree()` call with
`#if (configSUPPORT_DYNAMIC_ALLOCATION == 1)`, as is already done for the timer callback.

### Workaround

Define `pvPortMalloc()` (returning NULL) and `vPortFree()` in the application as traps that report any
call. With Arm Compiler the linker then removes them (no remaining reference). With GCC they are linked
and are never called. `configUSE_OS2_THREAD_ENUMERATE 0` removes one of the two users.
