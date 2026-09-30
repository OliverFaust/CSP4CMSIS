# Stage 2: NUCLEO-G474RE (STM32CubeMX/CubeIDE, ST's CMSIS-RTOS2 wrapper)

Stage 2 of `docs/hardware_test_plan.md`: the regression suite (`tests/fvp_sse300/bc_tests.cpp`) in a
project made exactly as in `Documentation/CSP4CMSIS_STM32CubeIDE.md`. Results: `docs/results_nucleo_g474.md`.

## Projects (STM32CubeIDE workspace, not in this repository)

| Project | Made from | Library in `lib/csp4cmsis/` | Application |
|---|---|---|---|
| `csp4cmsis_g474` | the guide, steps 1–4 (`csp4cmsis_g474.ioc` here) | v2.0.0 release source + 2.0.1 `csp_critical.h` | guide example `csp_app.cpp` |
| `csp4cmsis_g474_tests` | copy of `csp4cmsis_g474` | same | `Core/Src/bc_tests.cpp` (this repository's) |
| `csp4cmsis_g474_v1` | copy of `csp4cmsis_g474_tests` | v1.0.0 (`a789d2a`) + `Core/Inc/RTE_Components.h` shim (below) | same |
| `csp4cmsis_g474_tests_tp2` | copy of `csp4cmsis_g474_tests` | same as tests | same; `configTIMER_TASK_PRIORITY 2` (CubeMX default) |

Changes from the guide project for the suite:
- `Core/Src/csp_app.cpp` removed, `bc_tests.cpp` added. `main.c`: `USER CODE 0` declares
  `void csp_app_main_init(void);`, `USER CODE RTOS_THREADS` calls it (spawns the runner before
  `osKernelStart()`), `USER CODE 5` does not call `csp_app_main()`.
- `stm32g4xx_it.c`, `USER CODE BEGIN HardFault_IRQn 0`:
  `extern void bc_hardfault_report(void); bc_hardfault_report();` (the suite's fault report; CubeMX
  defines `HardFault_Handler` itself).
- **MCU/MPU G++ Compiler > Preprocessor**, in addition to the guide's four:
  `BC_SWI_IRQn=FMAC_IRQn`, `BC_SWI_HANDLER=FMAC_IRQHandler` (FMAC is unused), `BC_SWI_PRIO=6`
  (numerically ≥ 5), `BC_HARDFAULT_HANDLER=bc_hardfault_report`,
  `BC_BACKEND_NAME="FreeRTOS 10.3.1 (ST CMSIS_RTOS_V2 wrapper, STM32CubeG4 1.6.3)"`.
- **MCU/MPU G++ Linker > Miscellaneous > Other flags** (test T2):
  `-Wl,--wrap=osEventFlagsSet,--wrap=osThreadFlagsSet,--wrap=osSemaphoreRelease,--wrap=osSemaphoreAcquire,--wrap=osMessageQueuePut,--wrap=osMessageQueueGet,--wrap=osMessageQueueGetCount,--wrap=osMessageQueueGetSpace,--wrap=osMutexAcquire,--wrap=osMutexRelease`
- v1.0.0 only, `Core/Inc/RTE_Components.h` (1.0.0 includes it unconditionally; 2.0.1 does not):
  ```c
  #define CMSIS_device_header "stm32g4xx.h"
  ```

RAM (Debug): the suite uses 117 724 B of 131 072 B (`.data` + `.bss`, including T5's 32 KB channel and the 16 KB
FreeRTOS heap); no trimming was needed. Configurations: Debug (`-O0`) and Release (`-Os`, CubeIDE
default).

## Running

`run_stage2.sh` flashes each image with STM32CubeProgrammer (`STM32_Programmer_CLI`, SWD, reset after
programming) and logs the ST-LINK virtual COM port (115200 8N1) with `nucleo_run.py` until EOT or the
timeout. The port is opened before programming, and `nucleo_run.py` is its only reader. Logs:
`results/<date>_<config>.txt`, each with the ELF's SHA-256.
