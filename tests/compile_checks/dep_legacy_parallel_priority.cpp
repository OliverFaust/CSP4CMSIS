// EXPECT-WARNING: use CSP_DEFAULT_NETWORK_PRIORITY
// CSP_LEGACY_PARALLEL_PRIORITY is deprecated (2.1.0).
#include "csp/csp4cmsis.h"
osPriority_t f() { return csp::CSP_LEGACY_PARALLEL_PRIORITY; }
