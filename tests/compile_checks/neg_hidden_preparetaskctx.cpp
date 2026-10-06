// EXPECT-ERROR: private
// 3.0: CSProcess::prepareTaskCtx() is spawn plumbing for Run() only.
#include "csp/csp4cmsis.h"
struct P : csp::CSProcessStatic<256> { void run() override {} };
void f() { static P p; csp::CSProcess& c = p; (void)c.prepareTaskCtx(nullptr); }
