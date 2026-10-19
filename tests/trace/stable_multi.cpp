// Trace test: several processes awaiting the environment together (builds
// with CSP4CMSIS_TRACE only; without it this file is empty).
//
// csp4cmsis_trace_stable_multi() runs three processes, ROUNDS rounds each:
//   "Multi timed"    a new Alternative per round: guard 0 input multi_never
//                    (nobody writes), guard 1 timeout 3 ticks; ends by the
//                    timeout, then writes multi_link;
//   "Multi sampler"  a new Alternative per round with one guard: input
//                    multi_samples (Block, 2 elements), which only the
//                    interrupt writes;
//   "Multi sink"     reads multi_link (plain rendezvous).
// All three wait together (timed: ALT with a timeout; sampler: ALT without
// one, on an interrupt-fed channel; sink: plain read): stable points awaiting
// a timeout and an interrupt. The timeout releases "Multi timed" (and then
// the sink), the interrupt "Multi sampler". The interrupt is the platform's
// periodic one of stable_env.cpp (50 Hz, much longer than 3 ticks), which
// calls csp4cmsis_trace_stable_multi_isr() through
// csp4cmsis_trace_stable_env_isr().
// Expected (trace/README.md): ROUNDS PutFromISR on multi_samples; stable
// points with the offers of all three processes, and later of the sampler
// alone, all "awaiting environment"; none "deadlock". check_stable_multi.py
// checks them in decode.py's --stable output.
#if defined(CSP4CMSIS_TRACE)
#include "csp/csp4cmsis.h"
#include "trace_pool.h"

extern "C" void csp4cmsis_trace_env_timer(int on);

namespace {

constexpr uint32_t ROUNDS = 2u;

csp::BufferedChannel<uint32_t, 2> multi_samples;
csp::Channel<uint32_t> multi_never;
csp::Channel<uint32_t> multi_link;
volatile bool multi_active;
volatile uint32_t multi_puts;

class MultiTimed : public csp::CSProcessStatic<256> {   // select() with Event Recorder at -O0: more than 640 B
    csp::Chanin<uint32_t> never;
    csp::Chanout<uint32_t> link;

public:
    MultiTimed(csp::Chanin<uint32_t> n, csp::Chanout<uint32_t> l) : never(n), link(l) {}
    const char* name() const override { return "Multi timed"; }
    void run() override {
        uint32_t v = 0;
        for (uint32_t i = 0; i < ROUNDS; ++i) {
            csp::RelTimeoutGuard after3(csp::Ticks(3));
            csp::Alternative alt(never | v, after3);
            (void)alt.priSelect();
            link << i;
        }
    }
};

class MultiSampler : public csp::CSProcessStatic<224> {   // one-guard select(): more than 640 B at -O0
    csp::Chanin<uint32_t> in;

public:
    explicit MultiSampler(csp::Chanin<uint32_t> r) : in(r) {}
    const char* name() const override { return "Multi sampler"; }
    void run() override {
        uint32_t v = 0;
        for (uint32_t i = 0; i < ROUNDS; ++i) {
            csp::Alternative alt(in | v);
            (void)alt.priSelect();
        }
    }
};

class MultiSink : public csp::CSProcessStatic<160> {
    csp::Chanin<uint32_t> in;

public:
    explicit MultiSink(csp::Chanin<uint32_t> r) : in(r) {}
    const char* name() const override { return "Multi sink"; }
    void run() override {
        uint32_t v;
        for (uint32_t i = 0; i < ROUNDS; ++i) in >> v;
    }
};

} // namespace

extern "C" void csp4cmsis_trace_stable_multi_isr(void) {
    // putFromISR() on Block can fail while a get has not yet returned its
    // space token: count only the puts that succeed.
    if (multi_active && multi_puts < ROUNDS && multi_samples.isrWriter().putFromISR(multi_puts + 1u))
        multi_puts = multi_puts + 1u;
}

extern "C" void csp4cmsis_trace_stable_multi(void) {
    size_t off = 0;
    MultiTimed& timed = trace_pool_new<MultiTimed>(off, multi_never.reader(), multi_link.writer());
    MultiSampler& sampler = trace_pool_new<MultiSampler>(off, multi_samples.reader());
    MultiSink& sink = trace_pool_new<MultiSink>(off, multi_link.reader());
    multi_active = true;
    csp4cmsis_trace_env_timer(1);
    csp::Run(csp::InParallel(timed, sampler, sink), csp::ExecutionMode::TerminatingNetwork);
    csp4cmsis_trace_env_timer(0);
    multi_active = false;
    csp4cmsis_trace_pool_release();
}
#endif
