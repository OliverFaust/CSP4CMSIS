// --- public_task.h (Refactored for SPN) ---
#ifndef CSP4CMSIS_PUBLIC_TASK_H
#define CSP4CMSIS_PUBLIC_TASK_H

#include "csp4cmsis.h" // Includes CSProcess, ThreadFuncWrapper, etc.
#include "FreeRTOS.h"
#include "task.h"
#include <cstdio>

#ifndef TEST_STACK_SIZE_WORDS
#define TEST_STACK_SIZE_WORDS 8*256
#endif

// Define a default priority for user processes
#define CSP_DEFAULT_TASK_PRIORITY (configMAX_PRIORITIES - 1) 

extern "C" void ThreadFuncWrapper(void* pvParameters);

namespace csp {

/**
 * @brief Launches a single CSProcess as a FreeRTOS task, enforcing the Static Process Network (SPN) model.
 * * CRITICAL SPN REQUIREMENT: The CSProcess object and its associated FreeRTOS resources 
 * (TCB and Stack) MUST be allocated STATICALLY by the application (e.g., as a global or static local variable).
 * * The task creation is now implicitly tied to the static lifetime of the 'process' object.
 * * @param process Reference to the STATICALLY allocated CSProcess object.
 * @param priority The FreeRTOS priority for this task.
 */
inline void Run(CSProcess& process, UBaseType_t priority = CSP_DEFAULT_TASK_PRIORITY) {
    
    // API 1.2: process.stackWords()/taskPriority() override the call-site
    // defaults below when the process has an opinion; otherwise behavior
    // is byte-for-byte identical to pre-1.2.
    size_t stack = resolveStackWords(process, TEST_STACK_SIZE_WORDS);
    UBaseType_t effective_priority = resolveTaskPriority(process, priority);

    BaseType_t result = xTaskCreate(
        ThreadFuncWrapper,
        "CSP_PROC",
        stack,
        (void*)&process,
        effective_priority,
        NULL
    );

    if (result != pdPASS) {
        printf("FATAL ERROR: Failed to create FreeRTOS task for CSProcess. Check stack/heap/config.\r\n");
    }
}

/**
 * @brief Pauses the current process for a specified number of ticks.
 * Maps directly to FreeRTOS vTaskDelay.
 * @param ticks_to_sleep The number of RTOS ticks to pause.
 */
inline void SleepFor(TickType_t ticks_to_sleep) {
    vTaskDelay(ticks_to_sleep);
}

// NOTE: For full C++CSP compatibility, you would also define helper time functions here,
// but they are omitted for simplicity as per the plan.

} // namespace csp

#endif // CSP4CMSIS_PUBLIC_TASK_H