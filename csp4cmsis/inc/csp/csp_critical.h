#ifndef CSP4CMSIS_CRITICAL_H
#define CSP4CMSIS_CRITICAL_H

// Portable replacement for FreeRTOS's taskENTER_CRITICAL()/taskEXIT_CRITICAL()
// (and the _FROM_ISR variant) -- CMSIS-RTOS2 doesn't standardize a raw
// critical-section API, so csp4cmsis provides its own, built directly on
// CMSIS-Core BASEPRI intrinsics rather than FreeRTOS.
//
// BASEPRI, not PRIMASK: investigation (see docs/debugging-case-studies.md
// if this ships with one) confirmed multiple interrupts in real csp4cmsis
// deployments run above configMAX_SYSCALL_INTERRUPT_PRIORITY by design or
// vendor default (e.g. the console UART and an I2C/I3C sensor path both at
// the highest hardware priority on one target, an SE/MHU doorbell IRQ
// hardcoded just below it on another) -- a PRIMASK-based section would
// newly mask those during every csp4cmsis critical section. BASEPRI masks
// only priorities at or below the configured threshold, matching what
// FreeRTOS itself does on mainline Cortex-M, and leaves that above-
// threshold tier running exactly as it does today.
//
// __get_BASEPRI()/__set_BASEPRI_MAX()/__set_BASEPRI() are inherently
// interrupt-context-safe (unlike FreeRTOS's task/ISR-split API), so there
// is deliberately no separate _FromISR() pair here -- call the same two
// functions from ISR context too.

// RTE_Components.h + CMSIS_device_header: cmsis_os2.h alone does NOT pull
// in CMSIS-Core (confirmed by reading it -- it includes only <stdint.h>/
// <stddef.h>), so __NVIC_PRIO_BITS/__get_BASEPRI/__set_BASEPRI/
// __set_BASEPRI_MAX aren't visible through the includes the rest of
// csp4cmsis already uses. This is the same two-include idiom the
// application layer uses (e.g. main.cpp, BoardInit.cpp) to reach the
// target's core_cm*.h -- RTE_Components.h is generated per-project by the
// CMSIS-Toolbox build and defines CMSIS_device_header to the right
// device header for whatever MCU that project targets, so this stays
// portable across targets rather than csp4cmsis hardcoding one.
#include "RTE_Components.h"
#include CMSIS_device_header

// csp4cmsis's own threshold, independent of any specific RTOS's macro name
// -- keeps the library RTOS-agnostic. Projects define this to match
// whatever their underlying RTOS treats as configMAX_SYSCALL_INTERRUPT_
// PRIORITY (or equivalent), via a compiler -D flag -- not hardcoded here.
#ifndef CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY
#error "Define CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY to match your RTOS's critical-section threshold"
#endif

namespace csp::internal {

    inline uint32_t csp_enter_critical() {
        uint32_t saved = __get_BASEPRI();
        __set_BASEPRI_MAX(CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - __NVIC_PRIO_BITS));
        return saved;
    }

    inline void csp_exit_critical(uint32_t saved) {
        __set_BASEPRI(saved);
    }

} // namespace csp::internal

#endif // CSP4CMSIS_CRITICAL_H
