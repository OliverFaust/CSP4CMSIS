#ifndef HOST_CMSIS_COMPILER_H
#define HOST_CMSIS_COMPILER_H

// Host stand-in for CMSIS-Core's cmsis_compiler.h: only what
// csp4cmsis/src/alternative.cpp and csp_critical.h use. __CLZ is CMSIS 6's
// definition (cmsis_gcc.h, cmsis_armclang.h): __builtin_clz, 32 for 0.

#include <stdint.h>

static inline uint8_t __CLZ(uint32_t value) {
    if (value == 0U) return 32U;
    return (uint8_t)__builtin_clz(value);
}

#endif
