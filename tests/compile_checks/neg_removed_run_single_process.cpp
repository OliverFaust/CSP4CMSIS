// EXPECT-ERROR: no matching function
// Run(CSProcess&) removed in 3.0: Run(InParallel(p), ExecutionMode::StaticNetwork, priority).
#include "csp/csp4cmsis.h"
struct P : csp::CSProcessStatic<256> { void run() override {} };
void f() { static P p; csp::Run(p); }
