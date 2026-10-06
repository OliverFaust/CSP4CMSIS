// EXPECT-ERROR: no matching function || cannot convert || no viable conversion
// 3.0: Run() always takes an ExecutionMode (a priority alone is not one).
#include "csp/csp4cmsis.h"
struct P : csp::CSProcessStatic<256> { void run() override {} };
void f() { static P p; csp::Run(csp::InParallel(p), osPriorityLow); }
