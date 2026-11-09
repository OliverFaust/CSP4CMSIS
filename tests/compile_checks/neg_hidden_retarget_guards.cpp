// EXPECT-ERROR: private
// 3.2.0: the Alternative sets its channel guards' targets itself before every select.
#include "csp/csp4cmsis.h"
void f() { csp::Alternative alt; alt.retargetGuards(); }
