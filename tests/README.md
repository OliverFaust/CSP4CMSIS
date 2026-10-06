# CSP4CMSIS tests

| Directory | What |
|---|---|
| `fvp_sse300/bc_tests.cpp` | the regression suite (channels, ALT, timeouts, ISR writes, processes, `Run`, time conversion; T0–T31). Each test prints `RESULT <id>: PASS \| FAIL \| SKIP \| REPLACED -- <property>`, then `SUMMARY` and EOT. Test list, platform macros and results: `fvp_sse300/README.md` |
| `compile_checks/` | compile-time probes: removed APIs (`neg_*`), deprecations (`dep_*`), accepted code (`pos_*`), compiled with the flags of a suite build (`run_checks.py <compile_commands.json>`) |
| `doc_examples/` | the code blocks of the API reference page, compiled for ST's wrapper and as a pack (`doc_examples/README.md`) |
| `hw_nucleo_g474/` | the suite on a NUCLEO-G474RE with STM32CubeIDE and ST's CMSIS-RTOS2 wrapper (`hw_nucleo_g474/README.md`) |

The FVP harness used for the published results (Corstone-300 and MPS2 Cortex-M4 FVPs) is not public.
