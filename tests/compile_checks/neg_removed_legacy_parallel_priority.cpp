// EXPECT-ERROR: CSP_LEGACY_PARALLEL_PRIORITY
// Removed in 3.0: use CSP_DEFAULT_NETWORK_PRIORITY.
#include "csp/csp4cmsis.h"
osPriority_t f() { return csp::CSP_LEGACY_PARALLEL_PRIORITY; }
