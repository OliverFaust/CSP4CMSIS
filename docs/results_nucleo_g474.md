# CSP4CMSIS without packs: STM32CubeMX/CubeIDE and ST's CMSIS-RTOS2 wrapper (results)

**Date:** 2026-09-30, 2.0.1 results 2026-10-01. **Branch:** `release-2.0.1` (local). **Tools:** STM32CubeMX 6.17.0,
STM32Cube FW_G4 V1.6.3 (FreeRTOS 10.3.1, ST `CMSIS_RTOS_V2`), STM32CubeIDE 2.1.0 (GNU Tools for STM32
14.3.rel1), Fast Models 11.28.32.

**Status: the NUCLEO-G474RE board runs are not done yet** (the board was not connected). Everything
below that says "FVP" ran on the MPS2 Cortex-M4 FVP (Armv7E-M, the G474's architecture) with ST's
wrapper and FreeRTOS 10.3.1 compiled from the firmware package. The board projects are built and the
run is one command (`tests/hw_nucleo_g474/run_stage2.sh`).

## 0. CSP4CMSIS 2.0.1 (timeouts without RTOS timers, `release-2.0.1`)

Suite with T20-T24 and the new T6 (`tests/fvp_sse300/README.md`); library sources as `5b1239c`. Logs:
`tests/fvp_sse300/results/v2.0.1/`.

| Target, configurations | 2.0.1 |
|---|---|
| Corstone-300 FVP: AC6 and GCC × FreeRTOS/RTX5 × `-O0`/`-O2`/`-Os`/heap-free (16) | PASS=29 (heap-free 30) FAIL=0 SKIP=0 REPLACED=4 in all 16; 224 sweep lines BUG=0 ANOMALY=0; T15 0 bad trials |
| MPS2 Cortex-M4 FVP: AC6 and GCC × FreeRTOS/RTX5 × `-O0`/`-O2` (8) | PASS=29 FAIL=0 in all 8; sweeps clean |
| MPS2 Cortex-M4 FVP, **ST's wrapper**, GCC: `-O0`, `-O2`, **heap-free** | PASS=29, 29, **30** (T19: 0 allocator trap calls; heap used 0 B) |
| MPS2 Cortex-M4 FVP, `configTIMER_TASK_PRIORITY 2`: Arm's and ST's adapter; 40: Arm's | PASS=29 FAIL=0 in all three |
| v1.0.0 positive control (MPS2, GCC, Arm's and ST's adapter) | PASS=12 FAIL=18 SKIP=3: the same 18 failures as before; T20-T23 pass (1.0.0 has these paths), T24 SKIP, T6 FAIL |
| Compile checks (17), GCC and AC6 flags | all as expected |

**Positive controls on 2.0.0** (tag `v2.0.0`, the same suite, MPS2 FVP, GCC; logs in
`results/v2.0.1/controls_2.0.0/`): timer task priority 2 with Arm's and with ST's adapter: **HardFault**
(CFSR 0x8200); priority 55, Arm's adapter: PASS=27 **FAIL=2**: T6 (201 RTOS timers) and T24 (the 10-tick
timeout after 60 ticks under a stale wakeup per tick). Before the waker was bounded to 50 wakeups,
T24 hung the 2.0.0 run (the liveness violation of the model's mutation M2, here in the 2.0.0 code).

One MPS2 log (`regression_2.0.1_FreeRTOS_AC6.txt`) records `dirty=1` at `f979e46`: the working tree had
uncommitted documentation only (no difference in `csp4cmsis/` or the suite between `f979e46` and
`3abc45e`).

## 1. Library change (2.0.1): `csp_critical.h` without `RTE_Components.h`

`1305df4`: `RTE_Components.h` is included only if `__has_include` finds it; otherwise
`CSP4CMSIS_DEVICE_HEADER` names the device header; with neither, `#error` naming the define.
`6a08e8e`: `csp4cmsis.mk` (Himax SDK fragment) puts only `inc/` on the include path.

Other pack-only assumptions: none in the sources. `alternative.cpp` includes `<cmsis_compiler.h>`
(CMSIS-Core, present in every CMSIS device package); `csp_rtos_static.h` includes the RTOS's own
headers (`FreeRTOS.h`, `rtx_os.h`), found through the project's RTOS include paths.
Found on the way: `inc/csp` on the include path (pack builds add it too) makes `<time.h>` resolve to
`csp/time.h`; checked with `-I inc/csp` and `#include <time.h>` / `<ctime>` (both pick up CSP4CMSIS's
header).

| Check | Result |
|---|---|
| Corstone-300 FVP, pack build, `899afce` and `76194a5` (suite port; branch `nopack-device-header`, now `1305df4`, `5f4dba3`) | 16/16 configurations as before: PASS=24 (heap-free 25) FAIL=0 SKIP=0 REPLACED=4 (AC6, GCC × `-O0`/`-O2`/`-Os`/heap-free × FreeRTOS, RTX5) |
| MPS2 Cortex-M4 FVP, pack build, `899afce` | 4/4: PASS=24 FAIL=0 (FreeRTOS, RTX5 × AC6, GCC) |
| Compile, no packs, STM32CubeG4 headers, `-I inc` only | all 8 sources, no warnings (`-Wall -Wextra`); without the define: the `#error` |
| Compile, no packs, Himax WE2 SDK headers (`WE2_device.h`, CMSIS-FreeRTOS 10.5.1) | all 8 sources (compile only; no Himax board run) |

## 2. ST's CMSIS-RTOS2 wrapper

Full comparison: `docs/st_cmsis_rtos2_wrapper.md`. FVP runs (logs in
`tests/fvp_sse300/results/mps2_m4_st_wrapper/`):

| Configuration (ST wrapper, GCC) | Result |
|---|---|
| 2.0 `-O0`, timer task priority 55 | PASS=23 **FAIL=1 (T6: 16 B heap per `RelTimeoutGuard`)** SKIP=0 REPLACED=4; sweeps BUG=0 ANOMALY=0; T13/T13b 0 spins; T2 (no RTOS call under BASEPRI) PASS; T14 PASS |
| 2.0 `-O2`, priority 55 | same |
| v1.0.0 `-O0`, priority 55 | PASS=8 FAIL=18 SKIP=2: the same 18 failures as with Arm's adapter |
| 2.0 `-O0`, priority 2 (CubeMX default) | **HardFault** (CFSR 0x400) after T17 |
| 2.0 `-O0`, priority 2, **Arm's adapter** (11.3.0) | **HardFault**, same place: not a wrapper issue |
| 2.0 `-O0`, priority 40 (Arm's template default = the runner's `osPriorityHigh`), Arm's adapter | PASS=24 FAIL=0 |
| Guide example (`csp_app.cpp`, unmodified), priority 55 | `2000 messages, 0 errors, 0 timeouts: PASS` |
| Guide example, priority 2 | hangs after the first line (900 s simulated) |

**Timer task priority.** FreeRTOS runs timer start/stop/delete in the timer service task. T6 constructs
and destroys 200 `RelTimeoutGuard`s in a loop at `osPriorityHigh`; with the service task at priority 2
the delete commands are processed only when the runner blocks, on timer storage that has gone out of
scope and been reused. CSP4CMSIS 2.0 requires the service task at no lower priority than any thread
that uses timeouts (`CSP4CMSIS_Configuration.md` §8, new). The FVP and DK-E8 harnesses (55) and Arm's
CMSIS-FreeRTOS template (40, used by the DK-E8 example projects, whose CSP threads run below it) never
hit it; STM32CubeMX's default does, and so does the Himax project's `FreeRTOSConfig.h`
(`configTIMER_TASK_PRIORITY 2`).

## 3. STM32CubeMX/CubeIDE project (the guide)

**How the steps were checked.** The GUI was not operated in this session. Each GUI step was reproduced
through the same files the GUI writes, and the menu and field labels in the guide were taken from the
tools' own resources (CubeMX database and plugins, CubeIDE plugin descriptors):
- CubeMX: the `.ioc` with the guide's settings (`tests/hw_nucleo_g474/csp4cmsis_g474.ioc`), generated
  with the CubeMX command line (`STM32CubeMX -q`, `config load`, `project generate`).
- CubeIDE: "Convert to C++" reproduced as CubeIDE's handler does it (C++ nature, linker options to the
  G++ linker); Properties settings written to `.cproject`; builds with CubeIDE's headless builder.
A click-through of the guide in the GUI is still to be done (it would catch a label that differs on
screen, e.g. a renamed button).

| Check | Result |
|---|---|
| CubeMX defaults with CMSIS_V2 | `configSUPPORT_STATIC_ALLOCATION 1` (MEMORY_ALLOCATION "Dynamic / Static" is the only choice), `configMAX_PRIORITIES 56`, `configTIMER_TASK_PRIORITY 2`, `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5` |
| Convert to C++, then GENERATE CODE | CubeMX adds its include paths and defines to the G++ compiler (Convert alone does not) |
| CubeIDE G++ default | `-std=gnu++14`: CSP4CMSIS does not compile (`std::is_same_v`, `is_trivially_copyable_v`); GNU++17 builds |
| `CSP4CMSIS_DEVICE_HEADER="stm32g4xx.h"` typed as is | CubeIDE passes `'-DCSP4CMSIS_DEVICE_HEADER="stm32g4xx.h"'`: correct |
| Builds (Debug `-O0` / Release `-Os`) | 0 errors, 0 warnings: guide project 46 248 / 27 132 B text; suite 80 036 / 48 040 B text, 117 724 B of 131 072 B RAM (data + bss); v1.0.0 control 1 warning (unused test helper) |
| GENERATE CODE with USE_NEWLIB_REENTRANT disabled | modal "Warning: Code Generation" asking to enable it (read from the running CubeMX); enabled in the guide |
| Regeneration (with a changed FreeRTOS setting, and without changes) | `lib/csp4cmsis/`, `Core/Src/csp_app.cpp`, `USER CODE` in `main.c`, the source folder `lib/csp4cmsis/src`, the G++ include path, the four defines and GNU++17 all kept; only `FreeRTOSConfig.h` changed (and `.cproject` line endings) |

## 4. Board runs (pending)

`tests/hw_nucleo_g474/run_stage2.sh` runs, in this order: the guide example (`-O0`), the suite on 2.0
(`-O0`, `-Os`), the v1.0.0 control (`-O0`), and the suite with timer task priority 2. Expected from the
FVP: 23/1 with T6 failing (2.0), 8/18/2 (v1.0.0), a HardFault (priority 2). Not yet covered on hardware:
the hardware-only checks and the soak of stage 1.
