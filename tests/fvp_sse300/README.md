# BufferedChannel / ALT verification tests (Corstone-300 FVP)

`bc_tests.cpp` holds the tests behind `BUFFERED_CHANNEL_ANALYSIS.md` (repository root). They are
**test code**: they deliberately use `csp::internal` classes and FreeRTOS heap queries, and are not part
of the pack.

## Environment used

- Harness project: `helloworld_sse300` from the `arm_fvp_helloworld` repository, local branch
  **`csp4cmsis-wt-tests`**. That repository's `origin` is not ours; never push it.
- Target: Corstone-300 FVP (Fast Models 11.28.32, `FVP_Corstone_SSE-300_Ethos-U55`), Cortex-M55.
- Stack: Arm Compiler 6.24, CMSIS-Toolbox 2.14.1, CMSIS-RTOS2 over FreeRTOS 11.3.0 (`ARM::CMSIS-FreeRTOS`),
  heap_4 with 32 KB.
- CSP4CMSIS defines: `CSP4CMSIS_RTOS2_BACKEND_FREERTOS`, `CSP4CMSIS_STATIC_ALLOCATION`,
  `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5` (BASEPRI 0xA0).
- The FVP tick runs at about 312.5 Hz instead of the configured 100 Hz: `core_clk.mul` is 100 MHz while the
  software assumes 32 MHz. The tests use tick counts only, and no verdict depends on absolute time.

## How the harness builds against *this working tree*

The test branch changes only `hello.cproject.yml`:

```yaml
  packs:
    - pack: OliverFaust::CSP4CMSIS
      path: ../../../../../../home/of6/src/CSP4CMSIS   # = /home/of6/src/CSP4CMSIS
  ...
  groups:
    - group: CSP4CMSIS tests
      files:
        - file: ../../../../../../home/of6/src/CSP4CMSIS/tests/fvp_sse300/bc_tests.cpp
```

- `path:` makes csolution load `OliverFaust.CSP4CMSIS.pdsc` **directly from this clone**, so the component
  `OliverFaust::CSP4CMSIS:Core` compiles `csp4cmsis/src/*.cpp` and `csp4cmsis/inc/` of the working tree. The
  installed `OliverFaust::CSP4CMSIS@1.0.0` in `~/cmsis_packs` is not used.
  - Nothing is registered with `cpackget`, so there is no clash with the installed 1.0.0 of the same version.
  - csolution requires a relative path here (an absolute one only produces a portability warning).
- Verify: `grep -o '/home/of6/src/CSP4CMSIS/csp4cmsis/src/[a-z_]*.cpp' out/MPS3-Corstone-300/compile_commands.json`
  lists the working-tree sources. `hello.cbuild-pack.yml` does not list the pack, because local-path packs are
  not locked.
- `bc_tests.cpp` replaces the demo `application.cpp` and provides `csp_app_main_init()`.

## Rerun

```sh
cd <arm_fvp_helloworld>/helloworld_sse300
git switch csp4cmsis-wt-tests
source ../env.sh
cbuild hello.csolution.yml --packs --toolchain AC6 --rebuild
$FVP_BIN_DIR/FVP_Corstone_SSE-300_Ethos-U55 -a out/MPS3-Corstone-300/hello.axf \
    -C ethosu.num_macs=128 -f model_config_sse300.txt --simlimit 900 --stat
```

- The runner prints one `RESULT <id>: CONFIRMED | NOT REPRODUCED | PASS | FAIL -- ...` line per test,
  then sends EOT, which ends the FVP run after about 64 s of simulated time (about 5 minutes wall-clock).
  Reference output: `results/2026-09-26_fvp_run.txt`.
- Build with `-DTRACE_SWEEP=1` (add it to the cproject `define:`) to print every non-EARLY sweep trial.
- The run is deterministic: two runs of the same image produce identical output, apart from the telnet
  port numbers the FVP prints.

## Tests

| Id | Analysis item | What it does |
|---|---|---|
| T0 | control | Plain Block `BufferedChannel` read through an ALT; 10 values arrive |
| T1a | 1 | Phase sweep: a higher-priority writer's `output()` preempts an ALT reader at every instruction offset around `BufferedInputGuard::enable()` |
| T1b | 1 | Same for an ALT writer and `BufferedOutputGuard::enable()`, with a reader freeing the slot |
| T2 | 2 | `csp_enter_critical(); alt->wakeUp(bit); csp_exit_critical()`, exactly as in `_notifyReader()`; checks BASEPRI after `wakeUp()` and whether a higher-priority waiter ran inside the section |
| T3 | 3 | Phase sweep: two `KeepNewest` writers on a full channel; checks that both newest values survive |
| T4a | 4 | Two ALT writers on a full Block channel; the reader drains twice |
| T4b | 4 / 7 | An unrelated ALT whose output guard is never enabled calls `disable()` on it |
| T4c | 7 | A second `getOutputGuard()` call on a channel with a blocked ALT writer |
| T5 | 5 | Channel whose queue cannot be allocated (80 KB > 32 KB heap) |
| T6 | 6 | Heap use of `RelTimeoutGuard` and `Alternative`; 1000 constructions in a loop |
| T7a | 7 | ALT reader vs plain reader against an ALT writer blocked on a full channel |

### Phase-sweep method (T1a, T1b, T3)

1. The runner acts as the higher-priority aggressor. It aligns to a tick edge E0, releases the victim, and
   sleeps until the next edge E1.
2. The victim spins `k` iterations and then performs the operation under test.
3. At E1 the runner preempts the victim wherever it is and performs the conflicting operation.
4. A binary search finds the boundary `kb` where the aggressor starts acting before the victim's
   operation. The sweep then covers `k = kb-1500 … kb+50` in steps of one spin iteration, so every
   instruction offset of the victim's operation is hit. One tick is about 320,000 core cycles and
   `kb` ≈ 31,800, so one iteration is about 10 cycles.
5. The victim publishes a stage marker, so each hit also records where the victim was at E1.
6. A hang is detected by an ACK timeout (30 ticks). It is classified as a lost wakeup only if the channel
   really has data or space available. One successor operation then rescues the victim, so the sweep can
   continue.
