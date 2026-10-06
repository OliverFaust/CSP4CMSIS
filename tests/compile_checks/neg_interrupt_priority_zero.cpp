// EXPECT-ERROR: unshifted NVIC priority
// 3.0: 0 would mask nothing.
#undef CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY
#define CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY 0
#include "csp/csp4cmsis.h"
