# CSP4CMSIS Linker Map Analysis

This directory contains `csp4cmsis_map_report.py`, a standalone analysis
tool for the GNU ld linker map file produced when building a CSP4CMSIS
application. It exists to turn two of the book's central claims —
that CSP4CMSIS has a small, quantifiable memory footprint, and that it
performs **zero dynamic (heap) allocation** — from assertions in prose
into something a reader can reproduce and verify against a real build.

Both claims are discussed in the book's *Neuropathways* chapter,
"Resource Requirements: A Measured Example" (`sec:resource-requirements`).
This script is what that section's figures and the "On the zero heap
claim" paragraph are ultimately grounded in.

## What it does

Given a `.map` file (as produced by `-Wl,-Map=<project>.map`), the
script performs two independent kinds of analysis:

1. **Footprint report.** Walks the map file's per-object-file section
   entries and attributes FLASH and RAM usage to CSP4CMSIS specifically,
   separate from FreeRTOS, HAL/BSP code, TensorFlow Lite Micro, and the
   application's own code. Reports:
   - FLASH/RAM totals for CSP4CMSIS vs. the rest of the image, and as a
     percentage of each memory region.
   - A per-object-file breakdown (which `.o` files and which
     `libcsp4cmsis.a` members contribute what).
   - The largest individual CSP4CMSIS symbols, so a reader can see
     exactly where the bytes go (e.g. a process's stack, its
     `StaticTask_t`, a channel's buffer).
   - The stack/heap headroom implied by the `_end`/`_estack` linker
     symbols, matching the picture drawn in `sysmem.c`'s `_sbrk()`.

2. **Zero-heap audit.** Checks whether any CSP4CMSIS object file
   references a dynamic allocator — `pvPortMalloc`, `xTaskCreate`,
   `xEventGroupCreate`, `malloc`, C++ `operator new`/`new[]`, and
   related FreeRTOS/newlib entry points — as opposed to their static
   counterparts (`xTaskCreateStatic`, `xEventGroupCreateStatic`, etc.).
   This is a structurally different check from the footprint report: a
   symbol's *size* in `.bss`/`.data` says nothing about whether some
   unrelated function elsewhere in the same file also calls `malloc` at
   runtime. The audit instead walks the linker's own cross-reference
   data to answer that question directly, and reports, **per symbol**,
   one of three outcomes:
   - `NOT_IN_IMAGE` — the symbol never appears anywhere in this build,
     dynamic or not, so nothing was actually exercised for it.
   - `OK` — the symbol is present and called somewhere in the image,
     but never by CSP4CMSIS code.
   - `FAIL` — a CSP4CMSIS object file calls it, naming the offending
     file(s) directly.

   Distinguishing `NOT_IN_IMAGE` from `OK` matters: a single "PASSED"
   line can't tell you whether a symbol was genuinely checked or simply
   absent from the build. This script never collapses that distinction.

## Requirements

- Python 3.7+, standard library only.
- Optional: `arm-none-eabi-c++filt` (or plain `c++filt`) on `PATH`, for
  demangled C++ symbol names. Falls back to raw mangled names if not
  found — see the caveat on `operator new` below.

## Usage

```sh
python3 csp4cmsis_map_report.py path/to/project.map
python3 csp4cmsis_map_report.py project.map --top 30
python3 csp4cmsis_map_report.py project.map --pattern 'csp|Sender|Receiver'
python3 csp4cmsis_map_report.py project.map --no-demangle
```

`--pattern` overrides the default list of object-filename fragments
used to classify a file as CSP4CMSIS; adjust it if your project's
source layout differs from this repository's.


