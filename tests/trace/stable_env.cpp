// Trace test: a stable point awaiting the environment (builds with
// CSP4CMSIS_TRACE only; without it this file is empty).
//
// csp4cmsis_trace_stable_env() runs two processes: "Env sampler" takes
// SAMPLES elements from env_samples, a buffered channel that only an
// interrupt writes, and passes each to "Env sink" over the rendezvous channel
// env_link. Between interrupts both wait (sampler: env_samples empty; sink:
// env_link) and the idle thread runs: a stable point whose only way out is
// the interrupt. The platform provides the interrupt:
//   csp4cmsis_trace_env_timer(1)  start a periodic interrupt whose handler
//                                 calls csp4cmsis_trace_stable_env_isr();
//   csp4cmsis_trace_env_timer(0)  stop it.
// The test never deadlocks: every wait ends with the next interrupt. The
// handler also serves stable_multi.cpp and throttled_drop.cpp (which run
// after this test).
// Expected (trace/README.md): SAMPLES PutFromISR on env_samples; stable
// points whose offers are env_samples (read) and env_link (read), classified
// "awaiting environment" (env_samples is interrupt-fed); none of them
// "deadlock". check_stable_env.py checks them in decode.py's --stable output.
#if defined(CSP4CMSIS_TRACE)
#include "csp/csp4cmsis.h"
#include "trace_pool.h"

extern "C" void csp4cmsis_trace_env_timer(int on);

namespace {

constexpr uint32_t SAMPLES = 3u;

csp::BufferedChannel<uint32_t, 4> env_samples;
csp::Channel<uint32_t> env_link;
volatile uint32_t env_puts;

class EnvSampler : public csp::CSProcessStatic<160> {
    csp::Chanin<uint32_t> in;
    csp::Chanout<uint32_t> out;

public:
    EnvSampler(csp::Chanin<uint32_t> r, csp::Chanout<uint32_t> w) : in(r), out(w) {}
    const char* name() const override { return "Env sampler"; }
    void run() override {
        uint32_t v;
        for (uint32_t i = 0; i < SAMPLES; ++i) {
            in >> v;
            out << v;
        }
    }
};

class EnvSink : public csp::CSProcessStatic<160> {
    csp::Chanin<uint32_t> in;

public:
    explicit EnvSink(csp::Chanin<uint32_t> r) : in(r) {}
    const char* name() const override { return "Env sink"; }
    void run() override {
        uint32_t v;
        for (uint32_t i = 0; i < SAMPLES; ++i) in >> v;
    }
};

} // namespace

extern "C" void csp4cmsis_trace_stable_multi_isr(void);   // stable_multi.cpp (same interrupt)
extern "C" void csp4cmsis_trace_throttled_drop_isr(void); // throttled_drop.cpp (same interrupt)

extern "C" void csp4cmsis_trace_stable_env_isr(void) {
    if (env_puts < SAMPLES && env_samples.isrWriter().putFromISR(env_puts + 1u)) env_puts = env_puts + 1u;
    csp4cmsis_trace_stable_multi_isr();
    csp4cmsis_trace_throttled_drop_isr();
}

extern "C" void csp4cmsis_trace_stable_env(void) {
    size_t off = 0;
    EnvSampler& sampler = trace_pool_new<EnvSampler>(off, env_samples.reader(), env_link.writer());
    EnvSink& sink = trace_pool_new<EnvSink>(off, env_link.reader());
    csp4cmsis_trace_env_timer(1);
    csp::Run(csp::InParallel(sampler, sink), csp::ExecutionMode::TerminatingNetwork);
    csp4cmsis_trace_env_timer(0);
    csp4cmsis_trace_pool_release();
}
#endif
