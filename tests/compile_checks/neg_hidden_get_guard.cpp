// EXPECT-ERROR: private
// 3.0: guards come from bindings (in | v), not from Chanin::getGuard().
#include "csp/csp4cmsis.h"
void f() { static csp::Channel<int> ch; auto in = ch.reader(); int v; (void)in.getGuard(v); }
