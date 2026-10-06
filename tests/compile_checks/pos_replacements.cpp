// EXPECT-OK: the 3.0 API: replacements of every removed name, without a warning
#include "csp/csp4cmsis.h"
struct P : csp::CSProcessStatic<csp::CSP_MIN_STACK_WORDS> {
    void run() override {
        csp::SleepFor(csp::Milliseconds(5));
        csp::SleepFor(csp::Ticks(2));
        csp::SleepFor(csp::Seconds(1));
        csp::SleepFor(csp::Forever);
    }
};
static_assert(csp::Ticks(7).to_ticks() == 7, "Ticks");
static_assert(csp::Forever.to_ticks() == osWaitForever, "Forever");
void f() {
    static P p, q;
    csp::Run(csp::InParallel(p), csp::ExecutionMode::StaticNetwork, osPriorityHigh);
    csp::Run(csp::InParallel(q), csp::ExecutionMode::TerminatingNetwork, csp::CSP_DEFAULT_NETWORK_PRIORITY);
    static csp::Channel<int> a;                                           // was One2OneChannel, Any2OneChannel
    static csp::BufferedChannel<int, 4> b;                                // was BufferedOne2OneChannel
    static csp::BufferedChannel<int, 4, csp::BufferPolicy::KeepNewest> c; // was SamplingBufferedChannel
    static csp::SignalChannel s;                                          // was SignalChannel<>
    (void)a; (void)b; (void)c; (void)s;
    osPriority_t none = CSP_PRIORITY_UNSPECIFIED;                         // still without csp:: (was a macro)
    uint32_t hwm = csp::CSP_STACK_HWM_UNAVAILABLE + CSP_STACK_HWM_UNAVAILABLE;
    (void)none; (void)hwm;
    auto in = a.reader(); int v = 0; csp::RelTimeoutGuard to(csp::Forever);
    csp::Alternative alt(in | v, to);
    (void)alt.fairSelect();
}
