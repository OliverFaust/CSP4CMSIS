// EXPECT-ERROR: SIZE must be > 0
#include "csp/csp4cmsis.h"
csp::internal::BufferedChannel<int, 0> c;
