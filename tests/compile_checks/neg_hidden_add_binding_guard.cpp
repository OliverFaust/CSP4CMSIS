// EXPECT-ERROR: private
// 3.0: addBinding() takes bindings and RelTimeoutGuards only.
#include "csp/csp4cmsis.h"
void f(csp::internal::Guard* g) { csp::Alternative alt; alt.addBinding(g); }
