// EXPECT-WARNING: use BufferedChannel<T, SIZE> or SamplingBufferedChannel<T, SIZE, P>
// BufferedOne2OneChannel is deprecated (2.1.0).
#include "csp/csp4cmsis.h"
csp::BufferedOne2OneChannel<int, 4> ch;
