# Known Issues

## `OliverFaust.CSP4CMSIS.pdsc` was never run through `packchk`

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
