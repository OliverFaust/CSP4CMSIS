#!/usr/bin/env bash
# run_stage2.sh: flash and log every NUCLEO-G474RE configuration of stage 2 (docs/hardware_test_plan.md).
# Needs the CubeIDE projects built (Debug and Release) in $WS; see README.md.
set -uo pipefail
WS="${WS:-$HOME/STM32CubeIDE/workspace_csp4cmsis}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/results"; D="$(date +%F)"; mkdir -p "$OUT"
run() {  # run <project> <config> <label> <timeout s>
    local elf="$WS/$1/$2/$1.elf"
    [[ -f "$elf" ]] || { echo "missing $elf"; return 1; }
    echo "== $3"
    python3 "$HERE/nucleo_run.py" "$elf" "$OUT/${D}_$3.txt" "$4"
    grep -a -E '^SUMMARY|^csp_app: [0-9]|!!|TIMEOUT' "$OUT/${D}_$3.txt" | tail -3
}
# positive controls first
run csp4cmsis_g474_tests_200    Debug   2.0.0_ST-FreeRTOS_O0_control     1800   # expected: fails (HardFault or configASSERT trap, no SUMMARY)
run csp4cmsis_g474_v1           Debug   1.0.0_ST-FreeRTOS_O0_control     1800   # expected: the FVP's 18 FAILs
run csp4cmsis_g474_tests        Debug   2.0.1_ST-FreeRTOS_O0             1800
run csp4cmsis_g474_tests        Release 2.0.1_ST-FreeRTOS_Os             1800
run csp4cmsis_g474_tests_tp55   Debug   2.0.1_ST-FreeRTOS_O0_timerprio55 1800
run csp4cmsis_g474_tests_tp55   Release 2.0.1_ST-FreeRTOS_Os_timerprio55 1800
run csp4cmsis_g474_tests_noheap Debug   2.0.1_ST-FreeRTOS_O0_noheap      1800
run csp4cmsis_g474_tests_noheap Release 2.0.1_ST-FreeRTOS_Os_noheap      1800
run csp4cmsis_g474              Debug   2.0.1_example_O0                 120
