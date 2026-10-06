// EXPECT-WARNING: use Run(InParallel(p), ExecutionMode::StaticNetwork, priority)
// CSP_DEFAULT_TASK_PRIORITY is deprecated with Run(CSProcess&) (2.1.0).
#include "csp/csp4cmsis.h"
osPriority_t f() { return csp::CSP_DEFAULT_TASK_PRIORITY; }
