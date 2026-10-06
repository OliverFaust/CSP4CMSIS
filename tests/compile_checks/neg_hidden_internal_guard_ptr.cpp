// EXPECT-ERROR: private
// 3.0: a RelTimeoutGuard is passed to Alternative as it is.
#include "csp/csp4cmsis.h"
void f() { csp::RelTimeoutGuard to(csp::Time(5)); (void)to.internal_guard_ptr; }
