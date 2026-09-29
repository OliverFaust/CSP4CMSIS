// EXPECT-ERROR: deleted
#include "csp/csp4cmsis.h"
void f() { csp::RelTimeoutGuard a(csp::Time(1)); csp::RelTimeoutGuard b(a); }
