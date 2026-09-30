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
specific RTOS underneath. Version 2.0 is verified with the regression suite
on both the CMSIS-RTOS2-over-FreeRTOS adapter and native
[RTX5](https://github.com/ARM-software/CMSIS-RTX), in exactly these
configurations:

| Core (architecture) | Target | CMSIS-RTOS2 backends | Toolchains | Builds | Result |
|---|---|---|---|---|---|
| Cortex-M55 (Armv8.1-M Mainline) | Corstone-300 FVP (Fast Models 11.28.32) | FreeRTOS 11.3.0 via ARM CMSIS-FreeRTOS; Keil RTX5 5.9.1 | Arm Compiler 6.24, GCC 14.2.1 | `-O0`, `-O2`, `-Os`; heap-free `-O0` | all pass (PASS=24, heap-free 25; REPLACED=4) |
| Cortex-M55 (Armv8.1-M Mainline) | **Alif DK-E8 hardware**, RTSS-HP at 400 MHz | same | same | `-O0`, `-O2`, `-Os`; heap-free `-O0`; hardware-only checks; 67-pass soak | all pass |
| Cortex-M4F (Armv7E-M) | MPS2 Cortex-M4 FVP (Fast Models 11.28.32) | same | same | `-O0`, `-O2` | all pass |

Each target also has a v1.0.0 positive control (its known defects are detected; on the board for
RTX5 with Arm Compiler 6 only). Details:
`tests/fvp_sse300/README.md` (both FVPs) and `docs/hardware_results_dk_e8.md` (board).

**Not verified:** other cores (Cortex-M3, M7, M33, M85, …); Armv7E-M on real hardware. Armv6-M and
Armv8-M Baseline cores (Cortex-M0/M0+/M23) have no `BASEPRI`, which the critical section
(`csp_critical.h`) uses, so they are not supported (not attempted);
other CMSIS-RTOS2 implementations (ST's STM32Cube CMSIS-RTOS2 wrapper over FreeRTOS: FVP only so far,
with one difference, see [`docs/st_cmsis_rtos2_wrapper.md`](docs/st_cmsis_rtos2_wrapper.md)); IAR and Arm
LLVM (Clang) toolchains.

CSP4CMSIS deliberately stops at the boundary of a single CMSIS-RTOS2
instance. It does not manage multicore or inter-processor communication. For example, 
coordinating work across cores (e.g. Alif's RTSS-HP/RTSS-HE) is an
application-level concern, out of this library's scope.

## Design principles

- **No dynamic allocation of its own.** The library never calls an
  allocator (`malloc`, `operator new`, `pvPortMalloc`, …). With
  `CSP4CMSIS_STATIC_ALLOCATION` every RTOS object it creates also has a
  static control block, so it makes no dynamic RTOS allocation either.
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
- **No silent defaults where a wrong guess would be costly.** Which RTOS2
  backend you're building against, and the interrupt-priority threshold
  your critical sections need to respect, are both required, explicit
  configuration — CSP4CMSIS refuses to compile until you've set them
  deliberately.

## Getting started

CSP4CMSIS ships as a [CMSIS-Pack](https://open-cmsis-pack.github.io/Open-CMSIS-Pack-Spec/main/html/index.html).
Add it to your project:

```bash
cpackget add -a https://github.com/OliverFaust/CSP4CMSIS/releases/download/v2.0.0/OliverFaust.CSP4CMSIS.2.0.0.pack
```
`-a` accepts the pack's embedded MIT licence non-interactively; without it `cpackget` asks, and in a
script or CI job (no terminal input) it declines and installs nothing.

> Confirmed working: the `.pack` archive from the concrete, versioned
> release URL — not the bare `.pdsc`, and not `releases/latest/download/`.
> `cpackget add` treats a `.pdsc`-only URL as a local-file reference; the
> `.pack` is the installable unit that actually fetches over HTTPS.
> Update to a newer release deliberately by changing the version in the
> URL.

Without packs (STM32CubeIDE, vendor SDK makefiles), copy the source into your project instead:
[`Documentation/CSP4CMSIS_STM32CubeIDE.md`](Documentation/CSP4CMSIS_STM32CubeIDE.md) (needs 2.0.1's
`csp_critical.h`, see [`docs/CHANGES_2.0.1.md`](docs/CHANGES_2.0.1.md)).

Then reference the component in your `.cproject.yml`:

```yaml
components:
  - component: OliverFaust::CSP4CMSIS:Core
```

**Two project-level defines are required and one is optional** — see
[`Documentation/CSP4CMSIS_Configuration.md`](Documentation/CSP4CMSIS_Configuration.md)
for what each one means (required: the backend, `CSP4CMSIS_RTOS2_BACKEND_FREERTOS` or
`CSP4CMSIS_RTOS2_BACKEND_RTX5`, and `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY`; optional:
`CSP4CMSIS_STATIC_ALLOCATION`, needed for a heap-free system) and how to derive the right
value for your board, in particular `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY`, whose correct
value depends on your board's peripheral interrupt priorities, not (as
you might expect) on which RTOS backend you're using.

## Testing / examples

Board examples live in
[Alif-DK-E8-CSP4CMSIS](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS) (Alif DevKit-E8,
Cortex-M55). All use the published pack, pinned `OliverFaust::CSP4CMSIS@2.0.0`:

- [`csp4cmsis_alt_test`](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/main/csp4cmsis_alt_test)
  — ALT/select smoke test (two senders, one fair-select receiver) on RTX5.
- [`csp4cmsis_pack_test`](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/main/csp4cmsis_pack_test)
  — the same application on the FreeRTOS adapter.
- [`neuropathway`](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/main/neuropathway)
  — Sensor → Inference → Console network: IMU windows classified on the Ethos-U55 NPU (ExecuTorch),
  FreeRTOS.

Their move from 1.0.0 to 2.0.0 (no source change needed; board runs before and after) is recorded in
[`docs/migration-2.0/`](https://github.com/OliverFaust/Alif-DK-E8-CSP4CMSIS/tree/main/docs/migration-2.0).

The 2.0 regression suite and its results are in [`tests/fvp_sse300/`](tests/fvp_sse300/).

## Known limitations

See [`docs/known-issues.md`](docs/known-issues.md) — currently covers the
pack-tooling environment's broken `packchk` binary and how the pack was
validated without it.

## License

MIT — see [`LICENSE`](LICENSE).
