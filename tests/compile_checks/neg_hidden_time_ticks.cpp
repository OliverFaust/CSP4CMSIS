// EXPECT-ERROR: ticks
// 3.0: Time's tick count is read with to_ticks().
#include "csp/csp4cmsis.h"
unsigned f(csp::Time t) { return t.ticks; }
