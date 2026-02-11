#include "timer_utils.h"
#include "FreeRTOS.h"
#include "task.h"

/* =========================================================
   Internal state
   ========================================================= */

static TickType_t start_tick = 0;
static uint32_t cpu_freq_hz = 200000000;

/* ========================================================= */

void timer_init(void)
{
    cpu_freq_hz = SystemCoreClock;

    printf("Timer initialized (FreeRTOS tick mode)\r\n");
    printf("Tick period: %u ms\r\n", portTICK_PERIOD_MS);
    printf("CPU Frequency: %lu Hz\r\n", cpu_freq_hz);
}

/* ========================================================= */

void start_timer(void)
{
    start_tick = xTaskGetTickCount();
}

/* ========================================================= */

static uint32_t elapsed_ms_internal(void)
{
    TickType_t now = xTaskGetTickCount();

    /* unsigned subtraction = wrap safe */
    TickType_t delta = now - start_tick;

    return delta * portTICK_PERIOD_MS;
}

/* ========================================================= */

void stop_timer(void)
{
    uint32_t ms = elapsed_ms_internal();

    uint64_t cycles = ((uint64_t)ms * cpu_freq_hz) / 1000ULL;

    printf("\r\n=== Timer Results ===\r\n");
    printf("Elapsed cycles: %llu\r\n", cycles);
    printf("Elapsed time: %.3f us\r\n", (float)ms * 1000.0f);
    printf("Elapsed time: %.3f ms\r\n", (float)ms);
    printf("Elapsed time: %.3f s\r\n", (float)ms / 1000.0f);
    printf("CPU Frequency: %lu Hz\r\n", cpu_freq_hz);
    printf("=====================\r\n");
}

/* ========================================================= */

uint32_t stop_timer_cycles(void)
{
    uint32_t ms = elapsed_ms_internal();
    return ((uint64_t)ms * cpu_freq_hz) / 1000ULL;
}

float stop_timer_us(void)
{
    return (float)elapsed_ms_internal() * 1000.0f;
}

float stop_timer_ms(void)
{
    return (float)elapsed_ms_internal();
}

/* ========================================================= */
/* Compatibility functions                                  */
/* ========================================================= */

uint32_t get_current_cycles(void)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t ms = now * portTICK_PERIOD_MS;

    return ((uint64_t)ms * cpu_freq_hz) / 1000ULL;
}

void print_elapsed(uint32_t start_cycles)
{
    uint32_t now = get_current_cycles();
    uint32_t delta = now - start_cycles;

    float us = (float)delta / (cpu_freq_hz / 1000000.0f);

    printf("Elapsed: %.3f us\r\n", us);
}

void set_cpu_frequency(uint32_t freq_hz)
{
    cpu_freq_hz = freq_hz;
}

