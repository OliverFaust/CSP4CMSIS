# Release checklist

The steps used for 2.0.0, in order. Nothing is published before the "publish" steps.

## Prepare

1. `main` contains everything for the release; the working tree is clean.
2. Version: the first `<release version="X.Y.Z" date="YYYY-MM-DD">` in `OliverFaust.CSP4CMSIS.pdsc`,
   the component `Cversion`, the README install command, `docs/CHANGES_*.md` and the website patch
   (`docs/website/`) all name X.Y.Z. Set the release date to the planned release day.
3. Tests: the regression suite on the Corstone-300 and MPS2 Cortex-M4 FVPs
   (`tests/fvp_sse300/README.md`); hardware runs if library code changed.

## Build and check the pack

4. Tag `vX.Y.Z` (annotated) on the release commit; do not push it yet.
5. Build from a clean export of the tag: `git archive vX.Y.Z | tar -x -C <dir>`, then
   `python3 <dir>/scripts/build_pack.py <out>`.
6. `packchk` (built from source, see `docs/known-issues.md`): 0 errors, 0 warnings.
7. Compute the SHA-256 of the `.pack` and put it in the tag message and the release notes. (Until the
   build is byte-reproducible, see `docs/known-issues.md`, upload exactly the archive that was
   checked.)

## Publish

8. Push the tag.
9. Create the GitHub release `vX.Y.Z` from the tag, with the release notes, and attach:
   - `OliverFaust.CSP4CMSIS.X.Y.Z.pack` (the checked archive);
   - **`OliverFaust.CSP4CMSIS.pdsc` from the tag** (`git show vX.Y.Z:OliverFaust.CSP4CMSIS.pdsc`).
     The pdsc's `<url>` is `…/releases/latest/download/`, so `cpackget` looks for the pdsc there when
     it checks for updates.
10. Mark it as the latest release (not a pre-release).

## Verify as a user

11. `curl -L` both assets from `…/releases/download/vX.Y.Z/` and from `…/releases/latest/download/`;
    the pack's SHA-256 matches.
12. In an empty `CMSIS_PACK_ROOT`: run the README's `cpackget add -a …` command, then build (and run)
    a small project that uses `OliverFaust::CSP4CMSIS@X.Y.Z`.
13. `cpackget update-index` then `cpackget list --updates`: the new version is found for
    `OliverFaust::CSP4CMSIS`.

## After the release

14. Apply the website patch (`docs/website/`) to oliverfaust.github.io and push it.
