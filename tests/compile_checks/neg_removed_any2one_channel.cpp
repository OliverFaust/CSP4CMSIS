// EXPECT-ERROR: Any2OneChannel
// Removed in 3.0: any Channel may have several writers.
#include "csp/csp4cmsis.h"
csp::Any2OneChannel<int> ch;
