# CSP4CMSIS

A CSP (Communicating Sequential Processes)-style concurrency library, which offers 
channels, ALT/select, and process composition, for
[CMSIS-RTOS2](https://arm-software.github.io/CMSIS_6/latest/RTOS2/index.html).

CSP4CMSIS lets you build embedded firmware as a network of communicating
processes, in the tradition of Hoare's CSP, running on any conforming
CMSIS-RTOS2 implementation. It's part of Oliver Faust's ongoing work on
formally-groundable, deterministic concurrency for embedded AI on Arm
Cortex-M.

## Portability

CSP4CMSIS calls only the standard `cmsis_os2.h` API — it does not assume a
specific RTOS underneath. It is verified with the regression suite on Arm's
CMSIS-RTOS2-over-FreeRTOS adapter, ST's STM32Cube CMSIS-RTOS2 wrapper over FreeRTOS and native
[RTX5](https://github.com/ARM-software/CMSIS-RTX), in exactly these configurations:

| Core (architecture) | Target | CMSIS-RTOS2 backends | Toolchains | Builds | Version: result |
|---|---|---|---|---|---|
| Cortex-M55 (Armv8.1-M Mainline) | Corstone-300 FVP (Fast Models 11.28.32) | FreeRTOS 11.3.0 via ARM CMSIS-FreeRTOS; Keil RTX5 5.9.1 | Arm Compiler 6.24, GCC 14.2.1 | `-O0`, `-O2`, `-Os`; heap-free `-O0` | 3.0.0: all pass (PASS=36, heap-free 37; REPLACED=4) |
| Cortex-M55 (Armv8.1-M Mainline) | **Alif DK-E8 hardware**, RTSS-HP at 400 MHz | same | same | `-O0`, `-O2`, `-Os`; heap-free `-O0`; hardware-only checks; 67-pass soak | 2.0.0: all pass; 3.0.0: `-O0` FreeRTOS (GCC) and RTX5 (Arm Compiler 6) pass; the three [example projects](#testing--examples) run on 3.0.0 |
| Cortex-M4F (Armv7E-M) | MPS2 Cortex-M4 FVP (Fast Models 11.28.32) | same | same | `-O0`, `-O2` | 3.0.0: all pass |
| Cortex-M4F (Armv7E-M) | MPS2 Cortex-M4 FVP | FreeRTOS 10.3.1 via **ST's STM32Cube CMSIS-RTOS2 wrapper** (STM32CubeG4 1.6.3) | GCC 14.2.1 | `-O0`, `-O2`, heap-free `-O0` | 3.0.0: all pass |
| Cortex-M4F (Armv7E-M) | **NUCLEO-G474RE hardware**, 170 MHz (STM32CubeMX/CubeIDE project) | same (ST's wrapper) | GNU Tools for STM32 14.3.1 | `-O0`, `-Os`, heap-free `-O0`/`-Os` | 3.0.0: all pass (PASS=36, heap-free 37) |

Changes since the full DK-E8 runs (2.0.0): timeout guards and build integration (2.0.1,
[`docs/CHANGES_2.0.1.md`](docs/CHANGES_2.0.1.md)); deprecations, fatal errors instead of silent failures,
rounded-up time conversion and 7 new tests (2.1.0, [`docs/CHANGES_2.1.0.md`](docs/CHANGES_2.1.0.md)); the stable
3.0 API, static allocation by default (3.0.0, [`docs/CHANGES_3.0.md`](docs/CHANGES_3.0.md)). 3.0.0 also passes
with the 3.0 defaults (no backend or allocation define) on both FVPs.
The FVP harness used for these results is not public ([`tests/README.md`](tests/README.md)). Each target also has a
v1.0.0 positive control (its known defects are detected; on the DK-E8 for RTX5 with Arm Compiler 6
only), and the 2.0.0 timeout defect is detected on the MPS2 FVP and the NUCLEO-G474RE. Details:
`tests/fvp_sse300/README.md` (FVPs), `docs/hardware_results_dk_e8.md` and
`docs/hardware_results_nucleo_g474.md` (boards).

**Not verified:** other cores (Cortex-M3, M7, M33, M85, …). Armv6-M and
Armv8-M Baseline cores (Cortex-M0/M0+/M23) have no `BASEPRI`, which the critical section
(`csp_critical.h`) uses, so they are not supported (not attempted);
other CMSIS-RTOS2 implementations; IAR and Arm LLVM (Clang) toolchains. ST's wrapper differs from Arm's
adapter in ways an application can notice: [`docs/st_cmsis_rtos2_wrapper.md`](docs/st_cmsis_rtos2_wrapper.md).

CSP4CMSIS deliberately stops at the boundary of a single CMSIS-RTOS2
instance. It does not manage multicore or inter-processor communication. For example, 
coordinating work across cores (e.g. Alif's RTSS-HP/RTSS-HE) is an
application-level concern, out of this library's scope.

## Design principles

- **No dynamic allocation of its own.** The library never calls an
  allocator (`malloc`, `operator new`, `pvPortMalloc`, …). By default
  (static allocation) every RTOS object it creates also has a static
  control block, so it makes no dynamic RTOS allocation either.
  Verified by the full test suite passing on FreeRTOS and RTX5 with RTOS
  dynamic allocation disabled. A *completely* heap-free system additionally
  needs RTOS configuration (and, for CMSIS-FreeRTOS, two workarounds) and
  care with the C library's own heap; see
  [`Documentation/CSP4CMSIS_Configuration.md`](Documentation/CSP4CMSIS_Configuration.md),
  sections 2 and 6.
- **Portable critical sections.** Where the library needs to protect
  internal state (every channel kind, ALT state, ISR writes), it uses a
  CMSIS-Core-based (`BASEPRI`) critical section rather than an RTOS-
  specific API — CMSIS-RTOS2 has no standardized critical-section
  primitive, so this is CSP4CMSIS's own portable mechanism.
- **No silent defaults where a wrong guess would be costly.** The
  interrupt-priority threshold your critical sections need to respect is
  required, explicit configuration, and a value that cannot be right (0, or
  a shifted value) does not compile. Where a default is safe it is chosen
  (static allocation, the detected backend); where it is not (both RTOS
  headers reachable), the build stops and says what to define. Errors it
  cannot recover from at run time (a process that cannot be started, a 17th
  ALT guard) stop in `csp4cmsis_fatal_error()` instead of failing silently.

## Getting started

CSP4CMSIS ships as a [CMSIS-Pack](https://open-cmsis-pack.github.io/Open-CMSIS-Pack-Spec/main/html/index.html).
Add it to your project:

```bash
cpackget add -a https://github.com/OliverFaust/CSP4CMSIS/releases/download/v3.0.0/OliverFaust.CSP4CMSIS.3.0.0.pack
```
`-a` accepts the pack's embedded MIT licence non-interactively; without it `cpackget` asks, and in a
script or CI job (no terminal input) it declines and installs nothing.

> Confirmed working: the `.pack` archive from the concrete, versioned
> release URL — not the bare `.pdsc`, and not `releases/latest/download/`.
> `cpackget add` treats a `.pdsc`-only URL as a local-file reference; the
> `.pack` is the installable unit that actually fetches over HTTPS.
> Update to a newer release deliberately by changing the version in the
> URL. To check for one: with cpackget 2.2.1, `cpackget update-index` reports a newer
> CSP4CMSIS release ("can be upgraded from … to …"); `cpackget list --updates` does not show it.

Without packs (STM32CubeIDE, vendor SDK makefiles), copy the source into your project instead:
[`Documentation/CSP4CMSIS_STM32CubeIDE.md`](Documentation/CSP4CMSIS_STM32CubeIDE.md) (needs 2.0.1 or
later: `csp_critical.h` without `RTE_Components.h`, see [`docs/CHANGES_2.0.1.md`](docs/CHANGES_2.0.1.md)).

Then reference the component in your `.cproject.yml`:

```yaml
components:
  - component: OliverFaust::CSP4CMSIS:Core
```

**One project-level define is required:** `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY`, the unshifted
NVIC priority your critical sections mask up to. Its correct value depends on your board's peripheral
interrupt priorities, not (as you might expect) on which RTOS backend you're using. Builds without packs
also define `CSP4CMSIS_DEVICE_HEADER`. Everything else has a default: static allocation of every RTOS
object (opt out with `CSP4CMSIS_DYNAMIC_ALLOCATION`), with the CMSIS-RTOS2 backend detected
automatically. Details:
[`Documentation/CSP4CMSIS_Configuration.md`](Documentation/CSP4CMSIS_Configuration.md).

## API stability

**3.x will not break source compatibility:** code that compiles with 3.0 compiles with every 3.x release
(3.x may add API and fix bugs). The API is everything in namespace `csp` outside `csp::internal`, the
fatal-error hook, the configuration defines and the version macros (`CSP4CMSIS_VERSION_MAJOR`, `_MINOR`,
`_PATCH`). Migrating from 2.x: [`docs/CHANGES_3.0.md`](docs/CHANGES_3.0.md).

## Testing / examples

Board examples live in
[Alif-DK-E8-CSP4CMSIS](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS) (Alif DevKit-E8,
Cortex-M55). All use the published pack, pinned `OliverFaust::CSP4CMSIS@3.0.0`, with only
`CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` defined:

- [`csp4cmsis_alt_test`](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/main/csp4cmsis_alt_test)
  — ALT/select smoke test (two senders, one fair-select receiver) on RTX5.
- [`csp4cmsis_pack_test`](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/main/csp4cmsis_pack_test)
  — the same application on the FreeRTOS adapter.
- [`neuropathway`](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/main/neuropathway)
  — Sensor → Inference → Console network: IMU windows classified on the Ethos-U55 NPU (ExecuTorch),
  FreeRTOS.

Their move from 1.0.0 to 2.0.0 (no source change needed; board runs before and after) is recorded in
[`docs/migration-2.0/`](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/main/docs/migration-2.0), the
move to 3.0.0 (defines removed, `SleepFor(Milliseconds(10))`; board runs against 2.0.0) in
[`docs/migration-3.0/`](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/main/docs/migration-3.0).

The 2.0 regression suite and its results are in [`tests/fvp_sse300/`](tests/fvp_sse300/).

## Known limitations

See [`docs/known-issues.md`](docs/known-issues.md) — currently covers the
pack-tooling environment's broken `packchk` binary and how the pack was
validated without it.

## License and Declaration

MIT — see [`LICENSE`](LICENSE).

Development of this project utilizes AI coding assistants for boilerplate generation, unit test creation, and architectural drafting. All core logic is manually reviewed and verified.
