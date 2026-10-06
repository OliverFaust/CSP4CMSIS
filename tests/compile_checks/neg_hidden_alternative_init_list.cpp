// EXPECT-ERROR: no matching || no viable || could not convert || cannot convert || implicitly-deleted copy constructor
// 3.0: no Alternative({guard pointers}); use Alternative(in | v, timeout).
#include "csp/csp4cmsis.h"
void f() { csp::RelTimeoutGuard to(csp::Time(5)); csp::Guard* g = &to; csp::Alternative alt({g}); (void)alt; }
