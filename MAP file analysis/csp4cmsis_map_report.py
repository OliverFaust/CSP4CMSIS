#!/usr/bin/env python3
"""
csp4cmsis_map_report.py
------------------------
Drop-in analysis of a GNU ld linker .map file (as produced by
STM32CubeIDE / arm-none-eabi-gcc with -Wl,-Map=<project>.map) to show how
much FLASH and RAM the CSP4CMSIS library itself is responsible for,
separate from HAL/BSP/FreeRTOS/user code.

Usage:
    python3 csp4cmsis_map_report.py path/to/project.map
    python3 csp4cmsis_map_report.py project.map --top 30
    python3 csp4cmsis_map_report.py project.map --pattern 'csp|Sender|Receiver'
    python3 csp4cmsis_map_report.py project.map --no-demangle

Requirements:
    - Python 3.7+, stdlib only.
    - Optional: arm-none-eabi-c++filt (or plain c++filt) on PATH for
      demangled C++ symbol names. Falls back to raw mangled names if
      not found.

What it does:
    1. Reads "Memory Configuration" to get FLASH/RAM origin+length.
    2. Walks "Linker script and memory map" and collects every
       (sub)section entry: section name, address, size, contributing
       object file -- plus bare symbol assignments (e.g. _end, _estack,
       _Min_Stack_Size) used by sysmem.c's _sbrk().
    3. Classifies each entry as CSP4CMSIS or "other" using a filename/
       symbol-name pattern (default covers the csp4cmsis core sources:
       alt_channel_sync, alternative, barrier, buffered_channel,
       channel_sync, sync_channel, csp_wrapper, glue, kernel,
       csp4cmsis_spn, application, console_process, camera_process,
       inference_process -- adjust with --pattern for your project).
    4. Prints:
       - FLASH/RAM totals for CSP4CMSIS vs the whole image
       - a per-object-file breakdown
       - the N largest individual CSP4CMSIS symbols
       - the stack/heap headroom implied by _end/_estack/_Min_Stack_Size,
         matching the picture drawn in sysmem.c's _sbrk() comment.
       - a zero-heap audit (if the map file has a Cross Reference Table,
         i.e. was linked with -Wl,--cref): for a fixed list of dynamic
         allocator symbols (pvPortMalloc, xTaskCreate, operator new,
         ...), reports for EACH one whether it's absent from the image,
         present but never called by CSP4CMSIS code, or -- a real
         finding -- called by CSP4CMSIS code, naming the offending
         object file(s). C++ symbols (operator new/new[]) are matched
         via demangled name, not raw mangled text, since the mangled
         form never appears as literal "operator new" in the map file.
"""

import argparse
import re
import shutil
import subprocess
import sys
from collections import defaultdict


# --- Default set of object-file name fragments considered "CSP4CMSIS".
# Matched case-insensitively against the object file path in each map
# entry. Extend/override with --pattern if your project adds more.
DEFAULT_CSP_FILE_FRAGMENTS = [
    "alt_channel_sync",
    "alternative",
    "barrier",
    "buffered_channel",
    "channel_sync",
    "sync_channel",
    "csp_wrapper",
    "glue",
    "kernel",
    "csp4cmsis_spn",
    "application",
    "console_process",
    "camera_process",
    "inference_process",
]

HEX = r"0x[0-9a-fA-F]+"

# Matches a fully-formed sub-section entry on one line:
#   .text._ZN3csp8internal...   0x08001234   0x28   ./obj/alternative.o
RE_ENTRY_ONE_LINE = re.compile(
    rf"^\s+(?P<section>\.\S+)\s+(?P<addr>{HEX})\s+(?P<size>{HEX})\s+(?P<obj>\S+)\s*$"
)

# Matches a sub-section name alone (entry wraps to the next line because
# the section name is too long):
#   .text._ZN3csp8internal14AltChanSyncBaseC2Ev
RE_SECTION_NAME_ONLY = re.compile(r"^\s+(?P<section>\.\S+)\s*$")

# Matches the continuation line for the above:
#                   0x08001234       0x28 ./obj/alternative.o
RE_ENTRY_CONT = re.compile(
    rf"^\s+(?P<addr>{HEX})\s+(?P<size>{HEX})\s+(?P<obj>\S+)\s*$"
)

# Matches a bare symbol definition/assignment (no size), e.g.:
#                 0x08004520                g_pfnVectors
#                 0x2000ff00                _estack = ORIGIN(RAM) + LENGTH(RAM)
RE_SYMBOL = re.compile(
    rf"^\s+(?P<addr>{HEX})\s+(?P<sym>[A-Za-z_.$][\w.$]*)\b"
)

# Matches "Memory Configuration" region rows:
#   FLASH            0x08000000         0x00080000         xr
RE_MEMCFG_ROW = re.compile(
    rf"^(?P<name>\w+)\s+(?P<origin>{HEX})\s+(?P<length>{HEX})\s+\S*\s*$"
)

TOP_SECTION_KIND = {
    ".text": "FLASH",
    ".isr_vector": "FLASH",
    ".rodata": "FLASH",
    ".ARM": "FLASH",
    ".init_array": "FLASH",
    ".fini_array": "FLASH",
    ".data": "RAM",
    ".bss": "RAM",
    ".noinit": "RAM",
    ".heap": "RAM",
    ".stack": "RAM",
}


# --- Cross-reference-table parsing (zero-heap verification) ---
# Only present in the map file if you link with -Wl,--cref in addition
# to -Wl,-Map=<project>.map -- GNU ld does not emit this table by default.

RE_XREF_SYM_AND_FILE = re.compile(r"^(?P<sym>\S+)\s+(?P<file>\S+)\s*$")
RE_XREF_FILE_ONLY = re.compile(r"^\s+(?P<file>\S+)\s*$")

# Plain C symbols are matched by exact equality against the raw (still
# mangled, but C symbols aren't mangled) name. C++ symbols demangle WITH
# their parameter list attached (e.g. "_Znwj" -> "operator new(unsigned
# int)"), so "operator new" / "operator new[]" below are matched as a
# PREFIX of the demangled form (must include the trailing "(" so
# "operator new(" never accidentally matches "operator new[](...)").
DEFAULT_HEAP_SYMBOLS = [
    "pvPortMalloc",
    "vPortFree",
    "xTaskCreate",
    "xEventGroupCreate",
    "xQueueCreate",
    "xSemaphoreCreateBinary",
    "xSemaphoreCreateCounting",
    "xSemaphoreCreateMutex",
    "malloc",
    "_malloc_r",
    "operator new",
    "operator new[]",
]


def parse_cross_reference_table(lines):
    """Parse the GNU ld 'Cross Reference Table' section. Returns
    {symbol_name: [file, file, ...]} -- every file listed under that
    symbol's block (definer and referencers alike; we don't need to
    tell them apart for a "does CSP4CMSIS code touch this symbol at
    all" check)."""
    xref = {}
    in_section = False
    current_sym = None
    for raw in lines:
        line = raw.rstrip("\n")
        if line.strip() == "Cross Reference Table":
            in_section = True
            continue
        if not in_section or not line.strip():
            continue
        if line.strip().startswith("Symbol") and "File" in line:
            continue

        m = RE_XREF_SYM_AND_FILE.match(line)
        if m and not line[0].isspace():
            current_sym = m.group("sym")
            xref.setdefault(current_sym, []).append(m.group("file"))
            continue

        m = RE_XREF_FILE_ONLY.match(line)
        if m and current_sym is not None:
            xref[current_sym].append(m.group("file"))
            continue

        current_sym = None
    return xref


def _file_is_csp(file_path, fragments):
    norm = file_path.replace("\\", "/").lower()
    if "libcsp4cmsis.a(" in norm or "/csp4cmsis/" in norm:
        return True
    stem = obj_basename_stem(file_path).lower()
    return any(stem == frag.lower() for frag in fragments)


def audit_zero_heap(xref, fragments, heap_symbols=None, demangler=None):
    """Full, auditable zero-heap check. Returns a list of dicts, one per
    symbol in heap_symbols, each with:
        symbol      -- the human-readable target name
        status      -- "NOT_IN_IMAGE" (never referenced by anything in
                        this build -- nothing was actually exercised),
                        "OK" (referenced, but never by CSP4CMSIS code),
                        or "FAIL" (referenced by CSP4CMSIS code)
        callers     -- every file (CSP4CMSIS or not) that references it
        csp_callers -- the subset of callers classified as CSP4CMSIS

    Unlike a single pass/fail summary, every symbol gets an explicit
    verdict, so "no findings" can't be mistaken for "nothing was
    checked" (e.g. because a symbol never appears anywhere in the
    linked image, dynamic or not).

    C++ symbols are matched by demangled name, since the mangled form
    (e.g. "_Znwj") never equals the literal string "operator new" --
    matching that literally, as an earlier version of this script did,
    silently never matches anything and gives a false sense of coverage.
    """
    heap_symbols = heap_symbols or DEFAULT_HEAP_SYMBOLS
    raw_syms = list(xref.keys())
    demangled = demangle_all(raw_syms, demangler) if demangler else {s: s for s in raw_syms}

    def matches(raw, target):
        if raw == target:
            return True
        dm = demangled.get(raw, raw)
        if target in ("operator new", "operator new[]"):
            return dm.startswith(target + "(")
        return dm == target

    results = []
    for target in heap_symbols:
        matched_raw = [raw for raw in raw_syms if matches(raw, target)]
        if not matched_raw:
            results.append({
                "symbol": target, "status": "NOT_IN_IMAGE",
                "callers": [], "csp_callers": [],
            })
            continue
        all_callers = sorted(set(f for raw in matched_raw for f in xref[raw]))
        csp_callers = sorted(set(f for f in all_callers if _file_is_csp(f, fragments)))
        results.append({
            "symbol": target,
            "status": "FAIL" if csp_callers else "OK",
            "callers": all_callers,
            "csp_callers": csp_callers,
        })
    return results


def classify_kind(section_name: str) -> str:
    """Map a (sub)section name like '.text._ZN3csp...' to FLASH/RAM/OTHER."""
    for prefix, kind in TOP_SECTION_KIND.items():
        if section_name == prefix or section_name.startswith(prefix + "."):
            return kind
    return "OTHER"


def find_demangler():
    for candidate in ("arm-none-eabi-c++filt", "c++filt"):
        path = shutil.which(candidate)
        if path:
            return path
    return None


def demangle_all(names, demangler_path):
    """Batch-demangle via c++filt (one process, newline-separated stdin)."""
    if not demangler_path or not names:
        return {n: n for n in names}
    try:
        proc = subprocess.run(
            [demangler_path],
            input="\n".join(names),
            capture_output=True,
            text=True,
            timeout=30,
        )
        out_lines = proc.stdout.splitlines()
        if len(out_lines) == len(names):
            return dict(zip(names, out_lines))
    except Exception:
        pass
    return {n: n for n in names}


def detect_format_problem(lines):
    """Return a human-readable error string if this doesn't look like a
    real GNU ld linker map file (i.e. produced with -Wl,-Map=<file>), or
    None if it looks fine. Catches the common mistake of pointing this
    script at `arm-none-eabi-objdump -h <elf>` output instead, which has
    top-level section sizes but none of the per-object-file or
    per-symbol detail this script depends on."""
    text = "".join(lines)
    has_mem_cfg = "Memory Configuration" in text
    has_script_map = "Linker script and memory map" in text
    if has_mem_cfg and has_script_map:
        return None

    looks_like_objdump_h = bool(
        re.search(r"^\s*Idx\s+Name\s+Size\s+VMA\s+LMA", text, re.MULTILINE)
    ) or bool(re.search(r"file format elf32", text))

    if looks_like_objdump_h:
        return (
            "error: this looks like `arm-none-eabi-objdump -h <elf>` output, not a\n"
            "linker map file. It only has top-level section sizes (.text, .data,\n"
            ".bss, ...) with no per-object-file or per-symbol breakdown, so this\n"
            "script cannot attribute anything to CSP4CMSIS from it -- that\n"
            "information simply isn't in this file.\n\n"
            "Please supply the actual linker map file instead, generated by adding\n"
            "-Wl,-Map=<project>.map to the link step (STM32CubeIDE/most\n"
            "arm-none-eabi-gcc toolchains do this automatically; look for a "
            "\"Memory Configuration\" section and a \"Linker script and memory map\"\n"
            "section near the top of the file to confirm you have the right one)."
        )

    return (
        "error: this doesn't look like a GNU ld linker map file -- no \"Memory\n"
        "Configuration\" or \"Linker script and memory map\" section was found.\n"
        "Please supply the .map file produced by -Wl,-Map=<project>.map."
    )


def parse_memory_configuration(lines):
    regions = {}
    in_section = False
    for line in lines:
        if line.strip() == "Memory Configuration":
            in_section = True
            continue
        if in_section:
            if line.strip().startswith("Linker script and memory map"):
                break
            m = RE_MEMCFG_ROW.match(line)
            if m and m.group("name").lower() != "name":
                regions[m.group("name")] = (
                    int(m.group("origin"), 16),
                    int(m.group("length"), 16),
                )
    return regions


def parse_map(path):
    """Returns (entries, symbols) where:
    entries = list of dicts: section, addr, size, obj
    symbols = list of dicts: addr, name
    """
    with open(path, "r", errors="replace") as f:
        lines = f.readlines()

    regions = parse_memory_configuration(lines)

    entries = []
    symbols = []

    # Find start of "Linker script and memory map"
    start = 0
    for i, line in enumerate(lines):
        if line.strip().startswith("Linker script and memory map"):
            start = i + 1
            break

    pending_section = None
    for line in lines[start:]:
        if line.strip().startswith("OUTPUT(") or line.strip().startswith(
            "Cross Reference Table"
        ):
            break

        m1 = RE_ENTRY_ONE_LINE.match(line)
        if m1:
            entries.append(
                {
                    "section": m1.group("section"),
                    "addr": int(m1.group("addr"), 16),
                    "size": int(m1.group("size"), 16),
                    "obj": m1.group("obj"),
                }
            )
            pending_section = None
            continue

        if pending_section is not None:
            m2 = RE_ENTRY_CONT.match(line)
            if m2:
                entries.append(
                    {
                        "section": pending_section,
                        "addr": int(m2.group("addr"), 16),
                        "size": int(m2.group("size"), 16),
                        "obj": m2.group("obj"),
                    }
                )
                pending_section = None
                continue
            else:
                # continuation didn't match (e.g. it was a symbol line) --
                # fall through and re-check this line normally below.
                pending_section = None

        m3 = RE_SECTION_NAME_ONLY.match(line)
        if m3 and not line.strip().startswith("*("):
            pending_section = m3.group("section")
            continue

        m4 = RE_SYMBOL.match(line)
        if m4:
            symbols.append(
                {"addr": int(m4.group("addr"), 16), "name": m4.group("sym")}
            )
            continue

    return regions, entries, symbols


def obj_basename_stem(obj: str) -> str:
    """Extract a bare, comparable identifier from an object-file reference.
    Handles both plain paths ('path/to/file.o') and archive-member syntax
    ('path/to/libfoo.a(member.o)'), returning just the filename stem
    ('file' / 'member') with any '.o' extension stripped."""
    m = re.search(r"\(([^()]+)\)\s*$", obj)
    base = m.group(1) if m else obj
    base = base.replace("\\", "/").rstrip("/").split("/")[-1]
    if base.lower().endswith(".o"):
        base = base[:-2]
    return base


def is_csp_entry(entry, symbol_names_by_addr, fragments):
    obj = entry["obj"]
    obj_norm = obj.replace("\\", "/").lower()

    # Strong signal: physically inside the CSP4CMSIS library or its
    # source tree. Trust this unconditionally, regardless of the
    # fragment list below -- it's a structural fact about the build,
    # not a guess based on a filename.
    if "libcsp4cmsis.a(" in obj_norm or "/csp4cmsis/" in obj_norm:
        return True

    # Otherwise require an EXACT match on the object file's basename
    # (extension stripped) against one of the configured fragments --
    # NOT a substring-anywhere-in-path match. Substring matching on
    # short, generic fragments like "kernel" or "glue" false-positives
    # on unrelated files that merely contain that text as part of a
    # longer name or directory, e.g. ".../freertos_kernel/tasks.o" or
    # "kernel_util.o" -- neither of which is CSP4CMSIS code.
    stem = obj_basename_stem(obj).lower()
    if any(stem == frag.lower() for frag in fragments):
        return True

    # Fall back to symbol-name check (mangled C++ names embed "3csp" for
    # the csp:: namespace) in case the object file name itself doesn't
    # give it away (e.g. unusual build layouts, LTO'd translation units).
    sym = symbol_names_by_addr.get(entry["addr"], "")
    if "3csp" in sym or "csp::" in sym:
        return True
    return False


def human(n):
    return f"{n:,} B"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map_file", help="Path to the linker .map file")
    ap.add_argument("--top", type=int, default=25, help="Show N largest CSP4CMSIS symbols (default 25)")
    ap.add_argument(
        "--pattern",
        default=None,
        help="Comma or |-separated list of object-filename fragments that count as "
        "CSP4CMSIS. Overrides the built-in default list.",
    )
    ap.add_argument("--no-demangle", action="store_true", help="Skip c++filt demangling")
    args = ap.parse_args()

    fragments = DEFAULT_CSP_FILE_FRAGMENTS
    if args.pattern:
        fragments = [p for p in re.split(r"[,|]", args.pattern) if p]

    try:
        with open(args.map_file, "r", errors="replace") as f:
            raw_lines = f.readlines()
    except FileNotFoundError:
        print(f"error: map file not found: {args.map_file}", file=sys.stderr)
        sys.exit(1)

    problem = detect_format_problem(raw_lines)
    if problem:
        print(problem, file=sys.stderr)
        sys.exit(1)

    try:
        regions, entries, symbols = parse_map(args.map_file)
    except FileNotFoundError:
        print(f"error: map file not found: {args.map_file}", file=sys.stderr)
        sys.exit(1)

    if not entries:
        print(
            "warning: no per-object section entries were found. This usually means\n"
            "the build wasn't compiled with -ffunction-sections -fdata-sections, so\n"
            "the linker couldn't attribute individual symbols to files. Per-object\n"
            "and per-symbol breakdowns will be empty; only region totals will show.",
            file=sys.stderr,
        )

    symbol_by_addr = {s["addr"]: s["name"] for s in symbols}

    demangler = None if args.no_demangle else find_demangler()
    if not args.no_demangle and demangler is None:
        print("note: c++filt not found on PATH; symbol names will be shown mangled.\n", file=sys.stderr)

    # --- Classify entries ---
    csp_entries = [e for e in entries if is_csp_entry(e, symbol_by_addr, fragments)]
    other_entries = [e for e in entries if e not in csp_entries]

    def totals(entry_list):
        t = defaultdict(int)
        for e in entry_list:
            t[classify_kind(e["section"])] += e["size"]
        return t

    csp_totals = totals(csp_entries)
    other_totals = totals(other_entries)
    all_totals = totals(entries)

    print("=" * 72)
    print("CSP4CMSIS Linker Map Report")
    print("=" * 72)
    print(f"Map file: {args.map_file}")
    print(f"Classifying as CSP4CMSIS by object-file match: {', '.join(fragments)}")
    print()

    # --- Region summary ---
    if regions:
        print("Memory regions:")
        for name, (origin, length) in regions.items():
            print(f"  {name:<8} origin=0x{origin:08X}  length={human(length)}")
        print()

    print("FLASH / RAM footprint (bytes):")
    print(f"{'':16}{'CSP4CMSIS':>14}{'Rest of image':>16}{'Total':>14}")
    for kind in ("FLASH", "RAM"):
        c = csp_totals.get(kind, 0)
        o = other_totals.get(kind, 0)
        t = all_totals.get(kind, 0)
        print(f"{kind:<16}{c:>14,}{o:>16,}{t:>14,}")
        if kind in regions or True:
            region_len = None
            # RAM/FLASH region name assumed to match kind; adjust if your
            # linker script uses different region names.
            for rname, (origin, length) in regions.items():
                if rname.upper() == kind:
                    region_len = length
                    break
            if region_len:
                pct = 100.0 * t / region_len if region_len else 0.0
                pct_csp = 100.0 * c / region_len if region_len else 0.0
                print(
                    f"{'':16}  -> {pct_csp:5.2f}% of {kind} region used by CSP4CMSIS "
                    f"({pct:5.2f}% used total, region = {human(region_len)})"
                )
    print()

    # --- Per-object breakdown ---
    if csp_entries:
        by_obj = defaultdict(lambda: defaultdict(int))
        for e in csp_entries:
            by_obj[e["obj"]][classify_kind(e["section"])] += e["size"]

        print("Per-object-file breakdown (CSP4CMSIS only):")
        print(f"{'Object file':<45}{'FLASH':>12}{'RAM':>12}")
        for obj, kinds in sorted(by_obj.items(), key=lambda kv: -sum(kv[1].values())):
            print(f"{obj:<45}{kinds.get('FLASH', 0):>12,}{kinds.get('RAM', 0):>12,}")
        print()

    # --- Largest individual symbols ---
    if csp_entries:
        print(f"Largest CSP4CMSIS symbols (top {args.top}):")
        sym_names = [symbol_by_addr.get(e["addr"], e["section"]) for e in csp_entries]
        demangled = demangle_all(sorted(set(sym_names)), demangler)

        rows = []
        for e in csp_entries:
            raw = symbol_by_addr.get(e["addr"], e["section"])
            rows.append((e["size"], classify_kind(e["section"]), demangled.get(raw, raw), e["obj"]))
        rows.sort(key=lambda r: -r[0])

        print(f"{'Size':>10}  {'Kind':<6} Symbol / section  (object file)")
        for size, kind, name, obj in rows[: args.top]:
            short_name = name if len(name) <= 80 else name[:77] + "..."
            print(f"{size:>10,}  {kind:<6} {short_name}  ({obj})")
        print()

    # --- Stack/heap headroom, matching sysmem.c's _sbrk() picture ---
    interesting = {"_end", "_estack", "_Min_Stack_Size", "_Min_Heap_Size", "_sdata", "_edata", "_sbss", "_ebss"}
    found = {s["name"]: s["addr"] for s in symbols if s["name"] in interesting}
    if "_end" in found and "_estack" in found:
        end = found["_end"]
        estack = found["_estack"]
        min_stack = found.get("_Min_Stack_Size")  # this is usually a size, not an address symbol
        print("Heap / stack layout (per sysmem.c's _sbrk() picture):")
        print(f"  _end     = 0x{end:08X}   (top of .data+.bss / start of heap)")
        print(f"  _estack  = 0x{estack:08X}   (top of RAM / initial MSP)")
        print(f"  heap+stack region available = {human(estack - end)}")
        print(
            "  (actual usable heap = that region minus the reserved MSP stack, i.e.\n"
            "   _Min_Stack_Size -- see sysmem.c's _sbrk() for the exact check.)"
        )
        print()

    print("=" * 72)
    print("Tip: rebuild with -ffunction-sections -fdata-sections -Wl,--gc-sections")
    print("(STM32CubeIDE does this by default in Release) to get one map entry per")
    print("symbol -- without it, only region totals above are meaningful.")
    print("=" * 72)

    # --- Zero-heap audit ---
    xref = parse_cross_reference_table(raw_lines)
    print("Zero-heap audit (dynamic allocator cross-references):")
    if not xref:
        print(
            "  note: no Cross Reference Table found in this map file. Re-link with\n"
            "  -Wl,--cref added alongside -Wl,-Map=<project>.map to enable this check."
        )
    else:
        results = audit_zero_heap(xref, fragments, demangler=demangler)
        print(f"  {'Symbol':<26}{'Status':<14}{'Callers (non-CSP4CMSIS)':<28}CSP4CMSIS callers")
        any_fail = False
        for r in results:
            if r["status"] == "NOT_IN_IMAGE":
                print(f"  {r['symbol']:<26}{'not in image':<14}"
                      f"{'(never referenced anywhere in this build)':<28}-")
                continue
            non_csp = [f for f in r["callers"] if f not in r["csp_callers"]]
            non_csp_str = ", ".join(non_csp) if non_csp else "-"
            csp_str = ", ".join(r["csp_callers"]) if r["csp_callers"] else "-"
            if len(non_csp_str) > 26:
                non_csp_str = non_csp_str[:23] + "..."
            print(f"  {r['symbol']:<26}{r['status']:<14}{non_csp_str:<28}{csp_str}")
            if r["status"] == "FAIL":
                any_fail = True
        print()
        if any_fail:
            print("  RESULT: FAILED -- see 'CSP4CMSIS callers' column above for the")
            print("  offending object file(s).")
        else:
            checked = [r["symbol"] for r in results if r["status"] != "NOT_IN_IMAGE"]
            print(f"  RESULT: PASSED for all symbols actually present in this image")
            print(f"  ({', '.join(checked) if checked else 'none of the checked symbols appear in this build'}).")
            not_present = [r["symbol"] for r in results if r["status"] == "NOT_IN_IMAGE"]
            if not_present:
                print(f"  Not present in this build at all, so not exercised by this check:")
                print(f"  {', '.join(not_present)}.")
    print()


if __name__ == "__main__":
    main()