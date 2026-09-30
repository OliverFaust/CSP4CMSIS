# CSP4CMSIS 2.0.1 (not released): builds without CMSIS packs

- **`csp_critical.h`:** includes `RTE_Components.h` only if it exists (`__has_include`). Without it
  (STM32CubeIDE, vendor SDK makefiles), `CSP4CMSIS_DEVICE_HEADER` names the device header, e.g.
  `"stm32g4xx.h"`; with neither, the build stops with an `#error`. Pack builds are unchanged.
- **`csp4cmsis.mk`** (Himax SDK fragment): only `inc/` on the include path. `inc/csp` there made
  `#include <time.h>` find `csp/time.h`.
- **Documentation:** `Documentation/CSP4CMSIS_STM32CubeIDE.md` (new: STM32CubeMX/CubeIDE step by step);
  `CSP4CMSIS_Configuration.md` §7 (builds without packs) and §8 (FreeRTOS timer task priority), and a note
  in §6 on ST's wrapper; `docs/st_cmsis_rtos2_wrapper.md` (new).
- **Test harness:** `bc_tests.cpp` builds without packs as well (`tests/hw_nucleo_g474/`).

No API or behaviour change. Library sources other than `csp_critical.h` are identical to 2.0.0.
Evidence: `docs/results_nucleo_g474.md`.
