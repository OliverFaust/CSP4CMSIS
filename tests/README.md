# CSP4CMSIS tests

| Directory | What |
|---|---|
| `fvp_sse300/bc_tests.cpp` | the regression suite (channels, ALT, timeouts, ISR writes, processes, `Run`, time conversion; T0–T31). Each test prints `RESULT <id>: PASS \| FAIL \| SKIP \| REPLACED -- <property>`, then `SUMMARY` and EOT. Test list, platform macros and results: `fvp_sse300/README.md` |
| `host/alt_select/` | host unit test of the ALT winner selection: runs the library's `AltScheduler::select()` (built for the host against stand-ins for CMSIS-RTOS2 and CMSIS-Core) with scripted guards and compares the winner with the 3.0.0 selection loop, for a table of hand-checked cases and every case of 1–16 guards (every offset, every number of enabled guards, every ready mask; claimed rounds). `run.sh` (g++, needs CMSIS's `cmsis_os2.h`) |
| `trace/` | Event Recorder tracing (`CSP4CMSIS_TRACE` builds only): `drop_policy.cpp` puts into full KeepOldest/KeepNewest channels; `check_drop_policy.py` checks the exact events (2 BufferedPut + 3 DroppedNew, 2 BufferedPut + 3 OverwroteOldest) in the CSV of `tools/trace/decode.py`; `demo_network.cpp` is a small named network (two producers, an ALT merger with a timeout and an interrupt-fed KeepNewest channel, a consumer) for the Perfetto view |
| `compile_checks/` | compile-time probes: removed APIs (`neg_*`), deprecations (`dep_*`), accepted code (`pos_*`), compiled with the flags of a suite build (`run_checks.py <compile_commands.json>`) |
| `doc_examples/` | the code blocks of the API reference page, compiled for ST's wrapper and as a pack (`doc_examples/README.md`) |
| `hw_nucleo_g474/` | the suite on a NUCLEO-G474RE with STM32CubeIDE and ST's CMSIS-RTOS2 wrapper (`hw_nucleo_g474/README.md`) |

The FVP harness used for the published results (Corstone-300 and MPS2 Cortex-M4 FVPs) is not public.
