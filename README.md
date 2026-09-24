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
specific RTOS underneath. This is verified, not just claimed: the library
has been built and hardware-tested, with matching runtime behavior, on
both:

- the CMSIS-RTOS2-over-FreeRTOS adapter, and
- native [RTX5](https://github.com/ARM-software/CMSIS-RTX)

on an Alif Ensemble E8 (Cortex-M55).

CSP4CMSIS deliberately stops at the boundary of a single CMSIS-RTOS2
instance. It does not manage multicore or inter-processor communication —
coordinating work across cores (e.g. Alif's RTSS-HP/RTSS-HE) is an
application-level concern, out of this library's scope.

## Design principles

- **No dynamic allocation of its own.** CSP4CMSIS never calls `operator
  new`/`operator delete` and performs no heap allocation internally — it's
  usable in a zero-heap system. Whether *your* application code allocates
  is entirely your own decision; see
  [`Documentation/CSP4CMSIS_Configuration.md`](Documentation/CSP4CMSIS_Configuration.md)
  for what that means in practice.
- **Portable critical sections.** Where the library needs to protect
  internal state (`BufferedChannel`, `putFromISR()`), it uses a
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

