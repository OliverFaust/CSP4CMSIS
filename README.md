# CSP4CMSIS

A CSP (Communicating Sequential Processes)-style concurrency library —
channels, ALT/select, and process composition — for
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
other CMSIS-RTOS2 implementations (e.g. ST's STM32Cube CMSIS-RTOS2 wrapper over FreeRTOS); IAR and Arm
LLVM (Clang) toolchains.

CSP4CMSIS deliberately stops at the boundary of a single CMSIS-RTOS2
instance. It does not manage multicore or inter-processor communication —
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
cpackget add https://github.com/OliverFaust/CSP4CMSIS/releases/latest/download/OliverFaust.CSP4CMSIS.pdsc
```

Then reference the component in your `.cproject.yml`:

```yaml
components:
  - component: OliverFaust::CSP4CMSIS:Core
```

**Three project-level defines are required** — see
[`Documentation/CSP4CMSIS_Configuration.md`](Documentation/CSP4CMSIS_Configuration.md)
for what each one means and how to derive the right value for your board,
in particular `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY`, whose correct
value depends on your board's peripheral interrupt priorities, not (as
you might expect) on which RTOS backend you're using.

## Testing / examples

This repo is the library alone — no board-specific test projects are
kept here, since exercising CSP4CMSIS means bringing up a real
CMSIS-RTOS2 target (device selection, BSP, Secure Enclave init, etc.),
which is board/vendor-specific content, not part of a portable library.

CSP4CMSIS's own RTOS2 migration, RTX5 validation, and pack-installability
testing (raw-source and packaged-component builds, both hardware-verified)
were all done on an Alif Ensemble E8 (Cortex-M55) DevKit, in a separate
repo that holds that board's bring-up projects.
<!-- TODO(OliverFaust): link the DK-E8 repo here once you've decided
     whether/how to make it public — not linked here since its
     name/location isn't confirmed from this pass. -->

## Known limitations

See [`docs/known-issues.md`](docs/known-issues.md) — currently covers the
pack-tooling environment's broken `packchk` binary and how the pack was
validated without it.

## License

MIT — see [`LICENSE`](LICENSE).

