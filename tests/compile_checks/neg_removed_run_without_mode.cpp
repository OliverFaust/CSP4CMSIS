// EXPECT-ERROR: no matching function
// 3.0: Run() always takes an ExecutionMode.
#include "csp/csp4cmsis.h"
struct P : csp::CSProcessStatic<256> { void run() override {} };
void f() { static P p; csp::Run(csp::InParallel(p)); }
