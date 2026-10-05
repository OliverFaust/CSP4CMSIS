# API reference examples: compile check

Every code example of the CSP4CMSIS API reference
(<https://oliverfaust.github.io/CSP4CMSIS/api>, source `CSP4CMSIS/api.md` in the website repository)
is compiled here against this repository's library (v2.0.1), in two configurations.

| Path | Content |
|---|---|
| `examples/NN_sM.cpp` | the page's `cpp` blocks, in page order (`sM` = section), extracted unchanged |
| `examples/NN_sM.synopsis` | the page's declaration-only blocks (marked `<!-- synopsis … -->` on the page) |
| `check.py` | extracts the blocks, checks that `examples/` matches the page, checks every synopsis line against `csp4cmsis/inc/csp/*.h`, and compiles the examples for ST's wrapper |
| `pack/` | csolution project: the examples with the CSP4CMSIS pack on the Cortex-M4 FVP device (ARMCM4), Arm's CMSIS-FreeRTOS |

An example passes if it compiles with `-Wall -Wextra` and no warning (`-Werror` in `pack/`).

## 1. ST's CMSIS-RTOS2 wrapper, STM32G4 (as in the book)

Needs an STM32CubeIDE project for the NUCLEO-G474RE with FreeRTOS (CMSIS_V2), e.g. a clone of
[nucleo-g474re_The_Process](https://github.com/OliverFaust/nucleo-g474re_The_Process): its `Core/Inc`
(`FreeRTOSConfig.h`, HAL configuration), `Drivers/` and `Middlewares/` are used; the CSP4CMSIS headers
come from this repository. Flags as in the book's CubeIDE projects (GNU++17, the four `CSP4CMSIS_*`
defines, `-fno-exceptions -fno-rtti`).

```bash
python3 check.py extract <website>/CSP4CMSIS/api.md          # after editing the page
python3 check.py run --page <website>/CSP4CMSIS/api.md \
    --st <nucleo-g474re project> \
    --st-gxx <STM32CubeIDE>/plugins/<gnu-tools-for-stm32 …>/tools/bin/arm-none-eabi-g++
```

## 2. CMSIS pack, Cortex-M4 FVP device

`pack/doc_examples.csolution.yml` takes the CSP4CMSIS pack from this repository's
`OliverFaust.CSP4CMSIS.pdsc` (`path: ../../..`), with ARM::CMSIS 6.3.0, ARM::CMSIS-FreeRTOS 11.3.0
and ARM::Cortex_DFP 1.2.0 installed in `CMSIS_PACK_ROOT`. Output is a static library: the check
compiles the examples, it does not run them. The solution pins `compiler: GCC@13.2.1`: cbuild otherwise
takes the newest registered GCC (`GCC_TOOLCHAIN_<version>`).

```bash
cd pack
export GCC_TOOLCHAIN_13_2_1=<Arm GNU Toolchain 13.2.rel1>/bin
cbuild doc_examples.csolution.yml --update-rte --rebuild
```

To check against the released pack instead, unpack
`OliverFaust.CSP4CMSIS.2.0.1.pack` and point `path:` at it.

## Result (2026-10-05, CSP4CMSIS v2.0.1)

- `check.py run`: `examples/` matches the page (13 blocks); both synopses found in the headers; all 11
  examples compile with ST's wrapper (STM32Cube FW_G4 V1.6.3, FreeRTOS 10.3.1) and GNU Tools for
  STM32 13.3.rel1.
- `pack/`: all 11 examples compile with Arm GNU 13.2.rel1, both with this repository's pdsc and with
  the released `OliverFaust.CSP4CMSIS.2.0.1.pack`. (The first version of the page also compiled with
  GCC 14.2.1, before the toolchain was pinned.)
