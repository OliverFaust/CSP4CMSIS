#!/usr/bin/env bash
# run_stage2.sh: flash and log every NUCLEO-G474RE configuration of stage 2 (docs/hardware_test_plan.md).
# Needs the four CubeIDE projects built (Debug and Release) in $WS; see README.md.
set -uo pipefail
WS="${WS:-$HOME/STM32CubeIDE/workspace_csp4cmsis}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/results"; D="$(date +%F)"
run() {  # run <project> <config> <label> <timeout s>
    local elf="$WS/$1/$2/$1.elf"
    [[ -f "$elf" ]] || { echo "missing $elf"; return 1; }
    echo "== $3"
    python3 "$HERE/nucleo_run.py" "$elf" "$OUT/${D}_$3.txt" "$4"
    grep -a -E '^SUMMARY|^csp_app: [0-9]|!!|TIMEOUT' "$OUT/${D}_$3.txt" | tail -3
}
run csp4cmsis_g474           Debug   example_O0               120
run csp4cmsis_g474_tests     Debug   v2_ST-FreeRTOS_O0        1800
run csp4cmsis_g474_tests     Release v2_ST-FreeRTOS_Os        1800
run csp4cmsis_g474_v1        Debug   v1.0.0_ST-FreeRTOS_O0    1800
run csp4cmsis_g474_tests_tp2 Debug   v2_ST-FreeRTOS_O0_timerprio2 1800
