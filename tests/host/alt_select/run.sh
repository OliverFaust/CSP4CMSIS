#!/usr/bin/env bash
# run.sh [library dir]: builds alt_select_test.cpp with the library's
# alternative.cpp for the host (g++ or $CXX) and runs it. Needs CMSIS's
# cmsis_os2.h: CMSIS_RTOS2_INCLUDE=<CMSIS pack>/CMSIS/RTOS2/Include, or
# CMSIS_PACK_ROOT with ARM::CMSIS 6 installed.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB="${1:-$HERE/../../../csp4cmsis}"
if [[ -z "${CMSIS_RTOS2_INCLUDE:-}" ]]; then
  ROOT="${CMSIS_PACK_ROOT:-$HOME/.cache/arm/packs}"
  CMSIS_RTOS2_INCLUDE="$(ls -d "$ROOT"/ARM/CMSIS/6.*/CMSIS/RTOS2/Include 2>/dev/null | sort -V | tail -1)"
fi
[[ -f "$CMSIS_RTOS2_INCLUDE/cmsis_os2.h" ]] || { echo "cmsis_os2.h not found: set CMSIS_RTOS2_INCLUDE" >&2; exit 2; }
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
${CXX:-g++} -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -DCSP4CMSIS_DYNAMIC_ALLOCATION -DCSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5 \
  '-DCSP4CMSIS_DEVICE_HEADER="host_device.h"' \
  -I"$HERE/host" -I"$LIB/inc" -I"$LIB/inc/csp" -I"$CMSIS_RTOS2_INCLUDE" \
  "$HERE/alt_select_test.cpp" "$LIB/src/alternative.cpp" -o "$OUT/alt_select_test"
"$OUT/alt_select_test"
