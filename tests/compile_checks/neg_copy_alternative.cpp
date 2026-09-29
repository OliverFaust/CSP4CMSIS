// EXPECT-ERROR: deleted
#include "csp/csp4cmsis.h"
void f(csp::Alternative& a) { csp::Alternative b(a); (void)b; }
