// --- time.h (CMSIS-RTOS2 migration) ---
#ifndef CSP4CMSIS_TIME_H
#define CSP4CMSIS_TIME_H

// cmsis_os2.h for osKernelGetTickFreq(); RTOS2 tick counts are plain
// uint32_t (no RTOS-specific tick type), unlike FreeRTOS's TickType_t.
#include "cmsis_os2.h"
#include <stdint.h>

#ifdef __cplusplus
namespace csp {

/**
 * @brief Represents a duration or absolute time point in a type-safe manner.
 * Encapsulates a raw RTOS2 tick count and provides conversion helpers.
 */
struct Time {
    // The internal representation is the raw tick count
    uint32_t ticks;

    // Default constructor for Time()
    Time() : ticks(0) {}

    // Constructor required for the Time unit helpers (e.g., Seconds())
    explicit Time(uint32_t t) : ticks(t) {}

    /**
    * @brief Converts the Time object into the raw tick count for RTOS2 API calls.
    */
    uint32_t to_ticks() const {
        return ticks;
    }
};

// ----------------------------------------------------
// C++CSP Style Time Unit Helpers
// ----------------------------------------------------

namespace internal {
    /// ticks = ceil(amount * tick_frequency / per_second), computed in 64 bits;
    /// 0 stays 0, any other duration is at least 1 tick; saturates at
    /// 0xFFFFFFFE (0xFFFFFFFF is osWaitForever, not a duration).
    inline uint32_t to_ticks_round_up(uint32_t amount, uint32_t per_second) {
        const uint64_t f = osKernelGetTickFreq();
        const uint64_t t = ((uint64_t)amount * f + (per_second - 1U)) / per_second;
        return t > 0xFFFFFFFEULL ? 0xFFFFFFFEUL : (uint32_t)t;
    }
}

/**
 * @brief A duration of `s` seconds, in ticks of the kernel tick frequency
 * (osKernelGetTickFreq(), a run-time call: CMSIS-RTOS2 has no compile-time
 * tick rate).
 */
inline Time Seconds(uint32_t s) {
    return Time(internal::to_ticks_round_up(s, 1U));
}

/**
 * @brief A duration of `ms` milliseconds, in ticks, rounded UP (2.1.0): a
 * non-zero duration is never shorter than requested, and never 0 ticks.
 * Milliseconds(0) is 0 ticks.
 */
inline Time Milliseconds(uint32_t ms) {
    return Time(internal::to_ticks_round_up(ms, 1000U));
}

} // namespace csp
#endif // __cplusplus

#endif // CSP4CMSIS_TIME_H
