# CSP4CMSIS 3.0.1: changes

Patch release. **No behaviour change, no API change:** code that builds with 3.0.0 builds with 3.0.1 and
behaves the same.

## Change

**ALT winner selection with `__CLZ`** (`AltScheduler::select()`, `csp4cmsis/src/alternative.cpp`). In
phase 3, while `disable()` re-verifies the guards enabled in the round, `select()` now builds
`ready_mask`: bit 31−k is set when the k-th enabled guard in fairness order was re-verified as ready.
Phase 4 then takes `order[__CLZ(ready_mask)]`, the first ready guard in fairness order, with one count
leading zeros (on Armv7-M and Armv8-M Mainline a single `CLZ` instruction) instead of a loop over the
guards; an empty mask starts a new round, as before. The winner is chosen among re-verified guards only,
never from the thread flags. A partner's claim still wins, by the unchanged loop over the enabled guards.
`static_assert(ALT_MAX_GUARDS <= 32)` (the mask has 32 bits; the limit is 16). The `ready[]` array is gone.

The protocol is unchanged: the ProB models in `docs/formal/` that state the selection rule
(`alt_owrv_extended.csp`, `alt_one_winner.csp`, `alt_timeout_deadline.csp`: a claimed guard wins, else
the first ready guard in order, else a new round) describe 3.0.1 as they described 3.0.0. They are not
changed.

## Verification

Results (2026-10-12; library at `62e43da` on `release-3.0.1`, which differs from the release commit only in
the version numbers and documentation):

- **Selection, host unit test** (new, `tests/host/alt_select/`): the library's `select()`, built for the
  host against stand-ins for CMSIS-RTOS2 and CMSIS-Core, with scripted guards, against the 3.0.0 loop:
  a table of 15 hand-checked cases, and every case of 1–16 guards × every offset × every number of
  guards enabled in the round × every ready mask over them: 5 902 462 cases (no guard ready 1 632, one
  11 492, several 5 884 850; 4 488 claimed rounds). All give the 3.0.0 winner; an empty mask starts a
  new round; phase 3 disables exactly the enabled guards in fairness order. Built with
  `-fsanitize=address,undefined`. The 3.0.0 library passes the same test; three mutants fail it (the
  last ready guard instead of the first; an off-by-one in the bit position; `order[0]` for any non-empty
  mask).
- **Instruction:** `select()` contains one `CLZ` at `-O2` for Armv7-M (Cortex-M3) and Armv7E-M
  (Cortex-M4) with Arm Compiler 6.24 and GCC 14.2.1, both as an object file and in the MPS2 Cortex-M4
  regression images; also in all four NUCLEO-G474RE images (`-O0` and `-Os`). Cortex-M0+ (Armv6-M):
  CSP4CMSIS does not build there, as before (no `BASEPRI` for `csp_critical.h`; README, "Not
  verified"). With a stand-in for the `BASEPRI` intrinsics, `alternative.cpp` compiles for Cortex-M0+:
  GCC calls libgcc's `__clzsi2`, Arm Compiler 6 inlines a software count.
- **FVP, the 30 configurations of 3.0.0** (Corstone-300 Cortex-M55 and MPS2 Cortex-M4; Arm Compiler
  6.24 and GCC 14.2.1; FreeRTOS 11.3.0 through Arm's adapter, Keil RTX5, FreeRTOS 10.3.1 through ST's
  STM32Cube wrapper; `-O0`/`-O2`/`-Os`; heap-free builds; timer task priorities 2 and 40): PASS=36
  FAIL=0 SKIP=0 REPLACED=4 in all 30 (PASS=37 with T19 in the 5 heap-free builds), with the same verdict
  for every test (T0–T31) as 3.0.0 in every configuration. The FVP harness is not public; its logs are
  not published.
- **NUCLEO-G474RE**, ST's wrapper, STM32CubeIDE 2.1.0 (GNU Tools for STM32 14.3.rel1), the 3.0.0
  projects with the library replaced: Debug (`-O0`) and Release (`-Os`) PASS=36, heap-free PASS=37,
  FAIL=0, the same verdict for every test as 3.0.0 (`tests/hw_nucleo_g474/results/2026-10-12_3.0.1_*`).
- **Compile checks** (`tests/compile_checks/`): all probes pass with Arm Compiler 6 and GCC on FreeRTOS,
  RTX5 and ST's wrapper; the same 205 results as 3.0.0.
- **API reference examples** (`tests/doc_examples`, page for 3.0.1): no synopsis drift; 11/11 compile
  with ST's wrapper and in the pack build (`OliverFaust::CSP4CMSIS@3.0.1` from this repository's pdsc).
- **Pack:** built with `scripts/build_pack.py` from the release branch; `packchk` (CMSIS-Toolbox 2.15):
  0 errors, 0 warnings.

Not rerun for 3.0.1: the Alif DK-E8 (3.0.0 results in `docs/dk_e8_results_3.0.0/`), the 3.0 defaults and
dynamic-allocation FVP configurations, and the v1.0.0/2.0.1 positive controls (the suite is unchanged).
