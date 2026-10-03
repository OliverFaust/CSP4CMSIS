// EXPECT-ERROR: deleted
#include "csp/csp4cmsis.h"
void f() { static csp::SamplingBufferedChannel<int, 4> a; auto b = a; (void)b; }
