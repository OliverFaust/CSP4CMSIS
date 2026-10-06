// EXPECT-ERROR: BufferedAny2OneChannel
// Removed in 3.0: any BufferedChannel may have several writers.
#include "csp/csp4cmsis.h"
csp::BufferedAny2OneChannel<int, 4> ch;
