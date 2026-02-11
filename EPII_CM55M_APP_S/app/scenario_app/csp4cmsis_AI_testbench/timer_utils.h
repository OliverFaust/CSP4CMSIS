#ifndef TIMER_UTILS_H
#define TIMER_UTILS_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize DWT cycle counter (call once at startup)
void timer_init(void);

// Start timing
void start_timer(void);

// Stop timing and print results
void stop_timer(void);

// Stop timing and return elapsed cycles
uint32_t stop_timer_cycles(void);

// Stop timing and return elapsed microseconds
float stop_timer_us(void);

// Stop timing and return elapsed milliseconds
float stop_timer_ms(void);

// Get current cycle count
uint32_t get_current_cycles(void);

// Print elapsed time given start cycles
void print_elapsed(uint32_t start_cycles);

// Set CPU frequency (default is 200MHz)
void set_cpu_frequency(uint32_t freq_hz);

#ifdef __cplusplus
}
#endif

#endif // TIMER_UTILS_H
