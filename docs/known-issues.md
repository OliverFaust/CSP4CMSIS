# Known Issues

## `RelTimeoutGuard` (ALT timeouts) on 2.0.0: crash, hang, postponed timeouts, heap use (fix: 2.0.1)

Only applications that put a `RelTimeoutGuard` into an `Alternative` are affected; channels, ALT without
timeouts, `Barrier` and `SleepFor()` are not. In 2.0.0 a timeout guard is a CMSIS-RTOS2 timer
(`osTimerNew()` per guard, started and stopped by every `select()` round, deleted with the guard). The
RTOS runs the timer's callback, and with FreeRTOS also the stop and delete, later, in its timer service
thread. Affected:

1. **FreeRTOS (Arm's CMSIS-FreeRTOS adapter and ST's STM32Cube wrapper): crash or hang** when
   `configTIMER_TASK_PRIORITY` is below the priority of a thread that uses a `RelTimeoutGuard`. The
   delete is then processed after the guard's storage has gone out of scope and been reused.
   Reproduced on the MPS2 Cortex-M4 FVP: the regression suite ends in a HardFault with either adapter,
   and a two-sender ALT-with-timeout network hangs. This includes:
   - **STM32CubeMX projects with the default priority 2**: every thread at `osPriorityNormal` or above;
   - Arm's CMSIS-FreeRTOS configuration template (40): threads above `osPriorityHigh`.
2. **FreeRTOS and RTX5: a late timer callback** when a thread that uses timeouts has a priority equal to
   or above the timer service thread (FreeRTOS `configTIMER_TASK_PRIORITY`; RTX5 `OS_TIMER_THREAD_PRIO`,
   default 40). If the timeout expires in the same tick as another guard of the same ALT fires, the
   callback can run after the guard was destroyed and write one byte into reused stack memory (on ST's
   wrapper it also reads its freed callback record). Found by source analysis; not reproduced.
3. **ST's STM32Cube CMSIS-RTOS2 wrapper: RTOS heap.** Its `osTimerNew()` allocates the callback record
   with `pvPortMalloc()` even for a static control block: each live `RelTimeoutGuard` holds 16 bytes of
   FreeRTOS heap, also with `CSP4CMSIS_STATIC_ALLOCATION` (no crash; the build is not heap-free).
4. **All backends, independently of any priority: a timeout can be postponed indefinitely.** Every
   `select()` round restarts the guard's timer with the full duration. A wakeup that does not complete the
   `select()` (a stale ALT flag, a partner that withdrew, a lost race) starts a new round, so wakeups
   arriving more often than the timeout keep it from ever expiring. Reproduced on the MPS2 Cortex-M4
   FVP with FreeRTOS and with RTX5 (one such wakeup per tick: a 10-tick timeout expired only after 60
   ticks, when the wakeups stopped; with unbounded wakeups, FreeRTOS run, the `select()` never returned)
   and with ProB.

Items 1 and 2 do not affect RTX5 when every thread that uses timeouts runs below `OS_TIMER_THREAD_PRIO`;
item 4 affects every configuration.

**Workaround on 2.0.0:** give the timer service thread a priority **strictly above** every thread that
uses a `RelTimeoutGuard`, e.g. FreeRTOS `configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 1)` (55 with
CMSIS-RTOS2; STM32CubeMX: FREERTOS > Config parameters > Software timer definitions >
TIMER_TASK_PRIORITY), RTX5 `OS_TIMER_THREAD_PRIO 55`, and run no thread that uses timeouts at that
priority. This removes items 1 and 2; the 16 bytes of heap per guard on ST's wrapper (item 3) remain.
There is no workaround for item 4.

**Fixed in 2.0.1:** timeout guards no longer use an RTOS timer. `select()` waits for its thread flags
with the remaining time to a deadline fixed when the `select()` starts, so there is no timer object,
callback or timer priority requirement, no heap use on any adapter, and no wakeup can postpone a
timeout (items 1 to 4).

## `cpackget` never offers 2.0.0 to users of 1.0.0 (1.0.0 pdsc)

The 1.0.0 pdsc's `<url>` is a placeholder, `https://github.com/YourOrg/CSP4CMSIS/releases/latest/download/`.
`cpackget` fetches `<url>/OliverFaust.CSP4CMSIS.pdsc` to look for newer versions, so with 1.0.0 installed
`cpackget update-index` fails for this pack ("bad request") and `cpackget list --updates` never shows
2.0.0. The published 1.0.0 pack cannot be changed.

**Upgrade by URL** instead: `cpackget add -a
https://github.com/OliverFaust/CSP4CMSIS/releases/download/v2.0.0/OliverFaust.CSP4CMSIS.2.0.0.pack`, then
pin `OliverFaust::CSP4CMSIS@2.0.0` (see "Upgrading the pack" in `docs/CHANGES_2.0.md`). From 2.0.0 on, the
pdsc's `<url>` is the real `…/releases/latest/download/`, where the release also carries the pdsc, so the
update check works for later versions.

## `scripts/build_pack.py` does not build byte-identical packs (planned fix: 2.0.1)

**What varies.** Two builds of the same commit have identical contents (same files, same bytes, same
order) but different SHA-256 values:
- the `include/` directory entry is written with `ZipFile.writestr(name, '')`, which stamps it with the
  **current time** (the build time); this alone changes the archive on every build;
- the file entries take the **modification time** of the files on disk, so they differ between a
  `git clone` (checkout time) and a `git archive` export (commit time for a commit, tagger time for an
  annotated tag).

The 2.0.0 release asset (`OliverFaust.CSP4CMSIS.2.0.0.pack`, SHA-256 `cad3c617…d5`) is therefore one
particular build. It was checked to contain exactly the files of tag `v2.0.0`; rebuilds from the tag
gave the same contents with other hashes.

**Planned fix (2.0.1):** give every entry a fixed timestamp (the commit time from
`git log -1 --format=%ct`, or `SOURCE_DATE_EPOCH` if set), with fixed permissions and sorted names, so
that a build from any checkout of a commit is byte-identical and the release hash can be reproduced.

## `OliverFaust.CSP4CMSIS.pdsc` was never run through `packchk` (resolved for 2.0.0)

**Resolved 2026-09-27.** The 2.0.0 pack passes `packchk` with **0 errors and 0 warnings**.

**Root cause of the crash.** The released Linux `packchk` binaries (1.4.2, 1.4.4, 1.4.5 and 1.4.6 were
tried; all crash) are statically linked:
- Xerces-C (the XML parser) calls `iconv_open()`.
- For a non-builtin encoding, the embedded glibc `dlopen()`s the *system's* gconv modules and shared
  glibc (`__gconv_find_shlib` → `__libc_early_init`), which crashes on this host's glibc 2.41.

Locale settings and `GCONV_PATH` do not avoid it.

**What worked:** building `packchk` from source (dynamically linked against the system libraries):

```sh
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/Open-CMSIS-Pack/devtools.git
cmake -G Ninja -S devtools -B devtools/build -DCMAKE_BUILD_TYPE=Release
ninja -C devtools/build packchk          # -> devtools/build/tools/packchk/linux-amd64/Release/packchk
python3 scripts/build_pack.py <out-dir> && cd <out-dir> && unzip OliverFaust.CSP4CMSIS.2.0.0.pack -d x
packchk --xsd <cmsis-toolbox>/etc/PACK.xsd \
        -i $CMSIS_PACK_ROOT/ARM/CMSIS/6.0.0/ARM.CMSIS.pdsc \
        -i $CMSIS_PACK_ROOT/ARM/CMSIS-RTX/5.9.1/ARM.CMSIS-RTX.pdsc \
        -i $CMSIS_PACK_ROOT/ARM/CMSIS-FreeRTOS/11.3.0/ARM.CMSIS-FreeRTOS.pdsc \
        -n packname.txt x/OliverFaust.CSP4CMSIS.pdsc
```

- **Build used:** devtools `ce6763d`, GCC 14.2, CMake 3.31.
- **The `-i` packs** provide the `CMSIS:CORE` and `CMSIS:RTOS2` components that the pack's condition
  requires. Without them `packchk` reports M317/M362 (unresolved dependencies), which says nothing about
  this pack.
- **The 1.0.0 pdsc** also had M387/M388 (descriptions over 128 characters, with unsupported characters);
  2.0.0 fixes them.
- **Consumption test:** the 2.0.0 archive's contents are byte-identical to the working tree, and a
  project consuming the extracted pack (`OliverFaust::CSP4CMSIS@2.0.0`) builds and runs (FVP demo, same
  output as the 1.0.0 reference apart from throughput).

The text below is the original (1.0.0) record.


`OliverFaust.CSP4CMSIS.pdsc` (repo root) and the `.pack` archives built from it
(`OliverFaust.CSP4CMSIS.<version>.pack`) have **not** been validated by
`packchk`, PackChk's semantic checks (condition resolution against real
installed packs, component consistency, file-reference completeness
beyond raw existence). This is a confirmed environment-level tool
defect, not a workaround choice or something left unfinished by
omission.

**What was confirmed instead:**

- **Schema-valid.** `xmllint --noout --schema PACK.xsd OliverFaust.CSP4CMSIS.pdsc`
  passes cleanly.
- **File-completeness, verified manually.** Every path the `.pdsc`'s
  `<files>` section references was cross-checked against the working
  tree, and (when building a `.pack` archive) the archive's extracted
  contents were diffed byte-for-byte against the working tree -- the
  manual substitute for what `packchk` would otherwise catch.
- **What was not checked:** condition resolution (whether
  `CMSIS:RTOS2`/`CMSIS:CORE` actually resolve against real installed
  packs), component consistency, and any other semantic check `packchk`
  performs beyond file existence.

**Why `packchk` can't run here:** the installed `packchk` (CMSIS-Toolbox
2.14.0/2.14.1, Pack Verification 1.4.5) segfaults (exit 139) on every
`.pdsc` tested, including `ARM::CMSIS-RTX`'s own official, already-
published `.pdsc` -- proving the defect is in the binary, not in any
pack's content. Ruled out as fixable in this environment:

- **Not a version issue.** The latest available CMSIS-Toolbox release
  (2.15.0) ships the identical Pack Verification 1.4.5.
- **Not a corrupted local install.** A byte-identical (`sha256sum`-
  verified), freshly re-extracted copy of the binary from
  `tools/cmsis-toolbox.tar.gz` crashes identically.
- **Matches a known, unresolved (`wontfix`-tagged) upstream Linux
  `packchk` defect class** (Open-CMSIS-Pack/devtools#1048), though the
  publicly-documented symptom there (a dynamic-linker assertion) differs
  from what reproduces here (a raw SIGSEGV, no symbols, on a statically-
  linked stripped binary, confirmed via `gdb -batch -ex run -ex bt`) --
  likely a related-but-distinct manifestation of the same fragility
  class, not necessarily the identical root cause.
- **`gen_pack` is not a substitute.** It's a separate tool (not bundled
  with CMSIS-Toolbox), and it doesn't reimplement `packchk`'s checks --
  it just shells out to the same broken `packchk` binary internally.

**If this needs to be closed out properly:** try `packchk` on a
different OS or environment (Windows, or a container with a different
libc) where the binary may not exhibit this defect.

**Update: a real pack-consumption test (installing the pack with
`cpackget` and building a project against it as a packaged component,
rather than raw source) found two of the exact defects this gap was
worried about**, confirming the gap was real and not just theoretical:

- **`.pdsc` filename.** `cpackget add` rejected the pack outright:
  `"CSP4CMSIS.pdsc": pdsc file has wrong name, it should be
  <PackID>.pdsc`. The Open-CMSIS-Pack convention requires
  `<Vendor>.<PackName>.pdsc`; the file was just `CSP4CMSIS.pdsc`.
  Fixed by renaming to `OliverFaust.CSP4CMSIS.pdsc` (repo file and the
  `.pack` archive's internal copy both).
- **Missing include path.** A real build consuming
  `OliverFaust::CSP4CMSIS:Core` as a component (not raw source) failed:
  `fatal error: csp/csp4cmsis.h: No such file or directory`. The
  per-header `<file category="header">` entries only cause
  csolution/cbuild to auto-add `csp4cmsis/inc/csp/` to the include
  path (the directory each header physically lives in) -- but
  consumers reach the public API via `#include "csp/csp4cmsis.h"`,
  which needs `csp4cmsis/inc/` (the parent) on the path too. Fixed by
  adding an explicit `<file category="include"
  name="csp4cmsis/inc/"/>` entry to the `.pdsc`.

Both are exactly the class of problem `packchk`'s semantic checks
(file-reference/structure validation beyond raw existence) are meant
to catch, and neither showed up in schema validation or the earlier
manual file-completeness check -- they only surfaced once the pack was
actually installed (`cpackget add`) and built against as a real
component. `OliverFaust.CSP4CMSIS.1.0.0.pack` has been rebuilt with
both fixes and re-verified this way: clean Debug and Release builds,
and a hardware run on DK-E8 (20000 messages verified heap-free, no
errors) confirming the packaged component behaves identically to the
already-verified raw-source build. Condition resolution and component
consistency (the other things `packchk` would check) were also
exercised for real by this test -- `OliverFaust::CSP4CMSIS:Core`'s
`condition="CSP4CMSIS Core"` resolved correctly against ordinary
`CMSIS:RTOS2`/`CMSIS:CORE`-providing components in a real
`csolution.yml` -- so the remaining un-checked surface from
`packchk`'s absence is smaller than it was, though not certified
`packchk`-clean.
