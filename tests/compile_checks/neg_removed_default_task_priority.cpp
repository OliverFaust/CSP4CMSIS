// EXPECT-ERROR: CSP_DEFAULT_TASK_PRIORITY
// Removed in 3.0 with Run(CSProcess&).
#include "csp/csp4cmsis.h"
osPriority_t f() { return csp::CSP_DEFAULT_TASK_PRIORITY; }
