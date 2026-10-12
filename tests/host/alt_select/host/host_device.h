#ifndef HOST_DEVICE_H
#define HOST_DEVICE_H

// Host stand-in for the device header (CSP4CMSIS_DEVICE_HEADER): the BASEPRI
// intrinsics of csp_critical.h on a variable. The test is single-threaded.

#include "cmsis_compiler.h"

#define __NVIC_PRIO_BITS 4U

extern uint32_t host_basepri;
static inline uint32_t __get_BASEPRI(void) { return host_basepri; }
static inline void __set_BASEPRI(uint32_t v) { host_basepri = v; }
static inline void __set_BASEPRI_MAX(uint32_t v) {
    if (v != 0U && (host_basepri == 0U || v < host_basepri)) host_basepri = v;
}
static inline void __DSB(void) { }
static inline void __ISB(void) { }

#endif
