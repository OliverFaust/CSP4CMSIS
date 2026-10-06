# Using CSP4CMSIS with STM32CubeMX and STM32CubeIDE

This guide adds CSP4CMSIS to a new STM32CubeMX project for the NUCLEO-G474RE, builds it in STM32CubeIDE
as a C++ project, and runs a small CSP network. No CMSIS packs are involved: the library is copied into
the project as source.

**Tested with:** STM32CubeMX 6.17.0, STM32Cube FW_G4 V1.6.3 (FreeRTOS 10.3.1 with ST's CMSIS-RTOS2
wrapper), STM32CubeIDE 2.1.0 (GNU Tools for STM32 14.3.rel1), CSP4CMSIS 2.0.1 (the regression suite
also with 2.1.0, `tests/hw_nucleo_g474/README.md`). How each step was
checked is in `docs/results_nucleo_g474.md`.

**Requires CSP4CMSIS 2.0.1 or later.** CSP4CMSIS 2.0.0 includes `RTE_Components.h`, which only pack builds have,
and its timeout guards need FreeRTOS's timer task above every thread that uses them (CubeMX's default,
2, is not; `docs/known-issues.md`).

**Settings that matter**:
- **G++ language standard: GNU++17** (CubeIDE default: GNU++14, which does not compile CSP4CMSIS).
- **Four G++ defines** (step 3.4).

## 1. Create the project in STM32CubeMX

STM32CubeIDE 2.x no longer contains STM32CubeMX; the project is created in the standalone STM32CubeMX
and then imported.

1. **File > New Project**, tab **Board Selector**, part number **NUCLEO-G474RE**, select the board,
   **Start Project**. Answer **Yes** to "Initialize all peripherals with their default Mode?".
   This configures LPUART1 on the ST-LINK virtual COM port (category **BSP**, NUCLEO-G474RE: VCP), and
   the generated BSP code sends `printf` output there (115200 baud, 8N1).
2. **Pinout & Configuration > System Core > SYS > Timebase Source: TIM6.**
   FreeRTOS uses SysTick; with SysTick also as the HAL timebase, CubeMX warns at code generation.
3. **Pinout & Configuration > Middleware and Software Packs > FREERTOS > Interface: CMSIS_V2.**
4. In the FREERTOS **Configuration** panel:
   - **Config parameters > Memory management settings**: leave **Memory Allocation** at
     **Dynamic / Static** (the only choice with CMSIS_V2; static allocation is enabled). Set
     **TOTAL_HEAP_SIZE** as your application needs (the example and the tests use 16384; CSP4CMSIS
     itself uses none of it with `CSP4CMSIS_STATIC_ALLOCATION`).
   - **Advanced settings > Newlib settings > USE_NEWLIB_REENTRANT: Enabled.** CSP processes are threads,
     and several of them may call `printf`. If this stays disabled, CubeMX asks at every code generation
     "The USE_NEWLIB_REENTRANT must be set in order to make sure that newlib is fully reentrant …
     Do you still want to generate code?".
   - **Tasks and Queues > defaultTask**: double-click, **Stack Size (Words): 512**. The example runs its
     network from this task.
5. **Project Manager > Project**: **Project Name** (here `csp4cmsis_g474`), **Project Location**,
   **Toolchain / IDE: STM32CubeIDE**, **Generate Under Root: checked** (CubeIDE layout, sources under
   `Core/`).
6. **Project Manager > Code Generator > STM32Cube MCU packages and embedded software packs: Copy only the
   necessary library files.**
7. **GENERATE CODE**, then close the "Code Generation" dialog.

Result: `<project>/` with `Core/`, `Drivers/`, `Middlewares/`, the `.ioc`, `.project` and `.cproject`.
`Core/Inc/FreeRTOSConfig.h` contains `configUSE_NEWLIB_REENTRANT 1`, `configSUPPORT_STATIC_ALLOCATION 1`
and `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5`. The other FreeRTOS settings, including the timer
task priority, stay at CubeMX's defaults.

## 2. Import into STM32CubeIDE and make it a C++ project

1. **File > Import > General > Existing Projects into Workspace > Next**, **Select root directory**: the
   project folder, **Finish**. (Do not tick "Copy projects into workspace".)
2. **Project Explorer**: right-click the project **> Convert to C++**, confirm with **Continue**.
   CubeIDE moves the linker settings to the G++ linker. It does **not** give the G++ compiler the include
   paths and defines of the C compiler.
3. Back in STM32CubeMX (same `.ioc`): **GENERATE CODE** once more. For a C++ project CubeMX writes its
   include paths and defines into the G++ compiler settings as well.
4. In CubeIDE: select the project, **File > Refresh** (F5).

## 3. Add CSP4CMSIS

1. Download the library source: the release page
   <https://github.com/OliverFaust/CSP4CMSIS/releases/latest>, **Source code (zip)** or
   **(tar.gz)**. (Not the `.pack`, which is for pack-based builds.)
2. In the project folder, create `lib/` and copy the folder **`csp4cmsis/`** from the archive into it,
   together with the archive's `LICENSE` (MIT; keep it with the code):
   ```
   <project>/lib/csp4cmsis/inc/csp/*.h
   <project>/lib/csp4cmsis/src/*.cpp
   <project>/lib/csp4cmsis/LICENSE
   ```
   (`csp4cmsis.mk` and `README.md`, also in that folder, are not used by CubeIDE and can stay.)
   Refresh the project (F5).
3. **Project > Properties > C/C++ General > Paths and Symbols > Source Location > Add Folder…**, select
   **`lib/csp4cmsis/src`**, **OK**. (Adding `lib/csp4cmsis/src` rather than `lib/` keeps the headers,
   the makefile fragment and the README out of the build.)
4. **Project > Properties > C/C++ Build > Settings**, **Configuration: [All configurations]**,
   **Tool Settings**:
   - **MCU/MPU G++ Compiler > General > Language standard: GNU++17 (ISO C++17 + gnu extensions)
     (-std=gnu++17).**
   - **MCU/MPU G++ Compiler > Include paths > Include paths (-I) > Add… > Workspace…**, select
     **`lib/csp4cmsis/inc`**. The entry reads `"${workspace_loc:/${ProjName}/lib/csp4cmsis/inc}"`.
     Do **not** add `lib/csp4cmsis/inc/csp`: with it on the search path, `#include <time.h>` finds
     CSP4CMSIS's `time.h`.
   - **MCU/MPU G++ Compiler > Preprocessor > Define symbols (-D) > Add…**, one entry each:

     | Entry (typed as shown) | Why |
     |---|---|
     | `CSP4CMSIS_RTOS2_BACKEND_FREERTOS` | ST's CMSIS_V2 interface runs on FreeRTOS |
     | `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5` | = `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` in `FreeRTOSConfig.h`, **unshifted** (the library shifts it by the device's 4 priority bits: BASEPRI 0x50) |
     | `CSP4CMSIS_STATIC_ALLOCATION` | static control blocks for CSP4CMSIS's threads and semaphores |
     | `CSP4CMSIS_DEVICE_HEADER="stm32g4xx.h"` | the device header (CMSIS-Core, `__NVIC_PRIO_BITS`); with quotes, as shown |

     CubeIDE passes each define in single quotes (`'-DCSP4CMSIS_DEVICE_HEADER="stm32g4xx.h"'`), so the
     double quotes need no escaping. The defines are needed for the G++ compiler only; the library has no
     C sources.
   - **Apply and Close.**

What each define does, and how to choose `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` for other boards:
`Documentation/CSP4CMSIS_Configuration.md`.

## 4. Application code

CubeMX regenerates `main.c`, but keeps what is between `USER CODE BEGIN` and `USER CODE END`. Keep C++ code
in your own `.cpp` files and call it from those sections.

1. **File > New > Source File**, `Core/Src/csp_app.cpp`:
   ```cpp
   // Two senders, one receiver that ALTs over both channels with a timeout.
   #include "csp/csp4cmsis.h"
   #include <cstdio>

   using namespace csp;

   namespace {

   constexpr int MESSAGES = 1000;

   struct Message { int sender; int seq; };

   class Sender : public CSProcessStatic<256> {
       Chanout<Message> out;
       int id;
   public:
       Sender(Chanout<Message> o, int i) : out(o), id(i) {}
       void run() override {
           for (int i = 0; i < MESSAGES; ++i) out << Message{id, i};
       }
   };

   class Receiver : public CSProcessStatic<512> {
       Chanin<Message> inA, inB;
   public:
       Receiver(Chanin<Message> a, Chanin<Message> b) : inA(a), inB(b) {}
       void run() override {
           Message a{}, b{};
           int next[2] = {0, 0}, errors = 0, timeouts = 0;
           while (next[0] < MESSAGES || next[1] < MESSAGES) {
               RelTimeoutGuard timeout(Milliseconds(100));
               Alternative alt(inA | a, inB | b, timeout);
               switch (alt.fairSelect()) {
                   case 0: if (a.sender != 1 || a.seq != next[0]) ++errors; ++next[0]; break;
                   case 1: if (b.sender != 2 || b.seq != next[1]) ++errors; ++next[1]; break;
                   default: ++timeouts; break;
               }
           }
           printf("csp_app: %d messages, %d errors, %d timeouts: %s\r\n",
                  next[0] + next[1], errors, timeouts, errors == 0 ? "PASS" : "FAIL");
       }
   };

   } // namespace

   extern "C" void csp_app_main(void) {
       printf("\r\ncsp_app: CSP4CMSIS on STM32CubeMX FreeRTOS (CMSIS_V2)\r\n");
       static Channel<Message> chanA, chanB;
       static Sender   s1(chanA.writer(), 1), s2(chanB.writer(), 2);
       static Receiver r(chanA.reader(), chanB.reader());
       Run(InParallel(s1, s2, r));              // returns when all three have finished
       printf("csp_app: network finished\r\n");
   }
   ```
2. `Core/Src/main.c`, between the markers shown:
   ```c
   /* USER CODE BEGIN 0 */
   void csp_app_main(void);   /* Core/Src/csp_app.cpp */
   /* USER CODE END 0 */
   ```
   and in `StartDefaultTask()`:
   ```c
     /* USER CODE BEGIN 5 */
     csp_app_main();
     /* Infinite loop */
   ```
   (leave the generated `for(;;) { osDelay(1); }` after it).
3. **Make failed FreeRTOS assertions visible** (recommended). CubeMX's `configASSERT` disables interrupts
   and halts silently: the application just stops, with no output. In `Core/Inc/FreeRTOSConfig.h`, replace
   the line between the markers:
   ```c
   /* USER CODE BEGIN 1 */
   /* Report a failed assertion (file and line) before halting; CubeMX's default halts silently. */
   void vAssertCalled(const char *file, int line);
   #define configASSERT( x ) if ((x) == 0) { vAssertCalled(__FILE__, __LINE__); }
   /* USER CODE END 1 */
   ```
   and in `Core/Src/main.c` add `#include <stdio.h>` under `/* USER CODE BEGIN Includes */` and:
   ```c
   /* USER CODE BEGIN 4 */
   void vAssertCalled(const char *file, int line)
   {
     taskDISABLE_INTERRUPTS();
     printf("\r\nconfigASSERT failed: %s:%d\r\n", file, line);
     for (;;) { }
   }
   /* USER CODE END 4 */
   ```
   The BSP's `printf` writes to the virtual COM port by polling, so it works with interrupts masked.
   Example output (a CSP4CMSIS 2.0.0 timeout defect, `docs/known-issues.md`):
   `configASSERT failed: ../Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c:281`.
   Both edits are inside `USER CODE` sections and survive regeneration.

## 5. Build, flash, run

1. **Project > Build Project** (or the hammer). Expected: `0 errors, 0 warnings`.
2. Connect the NUCLEO-G474RE by USB (ST-LINK). **Run > Run As > STM32 C/C++ Application**, accept the
   default debug configuration (ST-LINK).
3. Open a serial terminal on the ST-LINK virtual COM port (Linux: `/dev/ttyACM*`, Windows: the
   "STMicroelectronics STLink Virtual COM Port"), 115200 8N1. Expected:
   ```
   csp_app: CSP4CMSIS on STM32CubeMX FreeRTOS (CMSIS_V2)
   csp_app: 2000 messages, 0 errors, 0 timeouts: PASS
   csp_app: network finished
   ```

## 6. Regenerating code in STM32CubeMX

Changing the configuration in CubeMX and pressing **GENERATE CODE** again keeps:
- `lib/csp4cmsis/` and your own files in `Core/Src` (CubeMX deletes only files it generated itself);
- the `lib/csp4cmsis/src` source folder, the G++ include path, the four defines and GNU++17 in
  `.cproject` (CubeMX rewrites its own include paths and defines and leaves other entries alone);
- the `USER CODE` sections of `main.c`.

It rewrites `FreeRTOSConfig.h`, so make FreeRTOS changes in CubeMX, not in the file (outside its
`USER CODE` sections).

## 7. What ST's CMSIS-RTOS2 wrapper changes for CSP4CMSIS

Details: `docs/st_cmsis_rtos2_wrapper.md`. In short:
- **Interrupt context is IPSR only.** The wrapper treats a thread that runs with interrupts masked
  (BASEPRI/PRIMASK raised) as a thread, not as interrupt context. Do not call CMSIS-RTOS2 functions
  (including CSP4CMSIS channel operations) from a thread while it has interrupts masked.
- Everything else CSP4CMSIS uses (thread flags, semaphores, static threads and semaphores) behaves the
  same as with Arm's adapter for CSP4CMSIS's use. CSP4CMSIS 2.0.1 uses no RTOS timers, so the wrapper's
  timer differences (heap per timer, asynchronous stop and delete) do not apply.

## Other IDEs and vendor SDKs (no packs)

The same four defines, the C++17 standard and the include path `…/csp4cmsis/inc` apply to any build
without packs. Name your device's CMSIS device header in `CSP4CMSIS_DEVICE_HEADER`: the header that
defines `__NVIC_PRIO_BITS` and includes the `core_cm*.h` file (e.g. `"stm32g4xx.h"`, Himax WE2:
`"WE2_device.h"`). In a makefile:
```make
CXXFLAGS += -std=gnu++17 -I$(CSP4CMSIS)/inc \
            -DCSP4CMSIS_RTOS2_BACKEND_FREERTOS -DCSP4CMSIS_STATIC_ALLOCATION \
            -DCSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5 \
            -DCSP4CMSIS_DEVICE_HEADER=\"WE2_device.h\"
```
The Himax SDK fragment `csp4cmsis/csp4cmsis.mk` adds the sources and the include path; put the defines in
the application's `.mk` (`APPL_DEFINES += …`).
