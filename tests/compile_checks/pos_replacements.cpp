// EXPECT-OK: the replacements of the 2.1.0 deprecations, without a warning
#include "csp/csp4cmsis.h"
struct P : csp::CSProcessStatic<256> { void run() override { csp::SleepFor(csp::Milliseconds(5)); csp::SleepFor(2u); } };
void f() {
    static P p, q;
    csp::Run(csp::InParallel(p), csp::ExecutionMode::StaticNetwork, osPriorityHigh);
    csp::Run(csp::InParallel(q), csp::ExecutionMode::TerminatingNetwork, csp::CSP_DEFAULT_NETWORK_PRIORITY);
    static csp::Channel<int> a;
    static csp::BufferedChannel<int, 4> b;
    static csp::SamplingBufferedChannel<int, 4, csp::BufferPolicy::KeepNewest> c;
    (void)a; (void)b; (void)c;
    csp::SleepFor(csp::Seconds(1));
}
