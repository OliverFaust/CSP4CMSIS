# Issue for ARM-software/CMSIS-FreeRTOS: ready to file (NOT filed)

**Title:** `clib_os.c` does not compile with `configSUPPORT_DYNAMIC_ALLOCATION = 0` (unguarded `xSemaphoreCreateMutex()` fallback)

**Checked against:**
- `main` @ `c3e5dc3c4531f02f47ed54cd05df0b3e16465155` (head on 2026-09-27);
- the released pack `ARM::CMSIS-FreeRTOS@11.3.0`.

The file `CMSIS/RTOS2/FreeRTOS/Source/clib_os.c` is identical in both.

### Description

`clib_os.c` implements the Arm C library's multithreading locks (Arm Compiler 6, full C library, not
MicroLIB). `_mutex_initialize()` first takes a mutex from a static pool and then falls back to dynamic
allocation:

```c
#if (OS_MUTEX_CLIB_NUM > 0)
  for (i = 0U; i < OS_MUTEX_CLIB_NUM; i++) {
    if (clib_mutex_id[i] == NULL) {
      /* Create mutex using static memory */
      clib_mutex_id[i] = xSemaphoreCreateMutexStatic(&clib_mutex_cb[i]);
      ...
#endif
  if ((*m == NULL) && (os_kernel_is_active())) {
    /* Create mutex using dynamic memory */
    *m = xSemaphoreCreateMutex();
  }
```

The fallback is not guarded. With `configSUPPORT_DYNAMIC_ALLOCATION = 0`, FreeRTOS's `semphr.h` does not
declare `xSemaphoreCreateMutex()`, so the file does not compile:

```
clib_os.c:204:10: error: call to undeclared function 'xSemaphoreCreateMutex'; ISO C99 and later do not
support implicit function declarations [-Wimplicit-function-declaration]
clib_os.c:204:8: error: incompatible integer to pointer conversion assigning to 'mutex' (aka 'void *')
from 'int' [-Wint-conversion]
```

(Arm Compiler 6.24, `configSUPPORT_STATIC_ALLOCATION = 1`, `configSUPPORT_DYNAMIC_ALLOCATION = 0`,
`configKERNEL_PROVIDED_STATIC_MEMORY = 1`, Cortex-M55.)

A fully static system (no heap implementation linked) therefore cannot use the CMSIS-RTOS2 adapter with
the Arm C library, although the static pool is sufficient for it.

### Suggested fix

```c
#if (configSUPPORT_DYNAMIC_ALLOCATION == 1)
  if ((*m == NULL) && (os_kernel_is_active())) {
    /* Create mutex using dynamic memory */
    *m = xSemaphoreCreateMutex();
  }
#endif
```

### Workaround

Force-include a header that defines `xSemaphoreCreateMutex()` as `((void *)0)`. Then only the static
pool (`OS_MUTEX_CLIB_NUM`, default 5) is used. With it, a heap-free build ran a 25-test regression suite
successfully on the Corstone-300 FVP.
