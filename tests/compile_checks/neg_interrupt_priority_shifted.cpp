// EXPECT-ERROR: unshifted NVIC priority
// 3.0: a shifted value (FreeRTOS's configMAX_SYSCALL_INTERRUPT_PRIORITY) is rejected.
#undef CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY
#define CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY 0xA0
#include "csp/csp4cmsis.h"
