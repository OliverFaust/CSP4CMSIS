// EXPECT-ERROR: BufferedOne2OneChannel
// Removed in 3.0 (deprecated in 2.1.0): use BufferedChannel<T, SIZE, P>.
#include "csp/csp4cmsis.h"
csp::BufferedOne2OneChannel<int, 4> ch;
