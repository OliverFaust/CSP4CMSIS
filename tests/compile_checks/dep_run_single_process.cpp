// EXPECT-WARNING: use Run(InParallel(p), ExecutionMode::StaticNetwork, priority)
// Run(CSProcess&, priority) is deprecated (2.1.0).
#include "csp/csp4cmsis.h"
struct P : csp::CSProcessStatic<256> { void run() override {} };
void f() { static P p; csp::Run(p); }
