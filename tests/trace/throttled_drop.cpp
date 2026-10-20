// Trace test: a slow consumer makes an interrupt-fed KeepOldest channel drop
// samples during operation (builds with CSP4CMSIS_TRACE only; without it this
// file is empty).
//
// csp4cmsis_trace_throttled_drop() runs two processes:
//   "Throttled sampler"  ROUNDS rounds of: a one-guard Alternative on
//                        throttled_samples (KeepOldest, 2 elements), which
//                        only the interrupt writes; then it forwards the
//                        sample on throttled_forward (plain rendezvous);
//   "Slow consumer"      reads throttled_forward ROUNDS times and sleeps
//                        1 tick after each read.
// The interrupt puts faster than the consumer takes: the sampler waits to
// write throttled_forward (backpressure), throttled_samples fills, and the
// interrupt's puts are dropped (DroppedNew) while the network still runs, not
// only after the sampler's last select. The interrupt is the platform's
// periodic one of stable_env.cpp, run at RATE_HZ for this test
// (csp4cmsis_trace_env_timer_rate()); csp4cmsis_trace_stable_env_isr() calls
// csp4cmsis_trace_throttled_drop_isr().
// Expected (trace/README.md): DroppedNew on throttled_samples before the
// sampler's last select, each at count == capacity; puts = gets + dropped +
// still buffered; the sampler waiting to write throttled_forward while drops
// occur. check_throttled_drop.py checks them in the CSV of decode.py.
#if defined(CSP4CMSIS_TRACE)
#include "csp/csp4cmsis.h"
#include "trace_pool.h"

extern "C" void csp4cmsis_trace_env_timer(int on);
extern "C" void csp4cmsis_trace_env_timer_rate(uint32_t hz);

static csp::BufferedChannel<uint32_t, 2, csp::BufferPolicy::KeepOldest> throttled_samples;
static csp::Channel<uint32_t> throttled_forward;

namespace {

constexpr uint32_t ROUNDS = 20u;
constexpr uint32_t RATE_HZ = 500u;   // of the timer's clock: several puts per 1-tick sleep at 100 Hz ticks

volatile bool throttled_active;
volatile uint32_t throttled_sample;

class ThrottledSampler : public csp::CSProcessStatic<256> {   // select() with Event Recorder at -O0: more than 640 B
    csp::Chanin<uint32_t> in;
    csp::Chanout<uint32_t> out;

public:
    ThrottledSampler(csp::Chanin<uint32_t> r, csp::Chanout<uint32_t> w) : in(r), out(w) {}
    const char* name() const override { return "Throttled sampler"; }
    void run() override {
        uint32_t v = 0;
        csp::Alternative alt(in | v);
        for (uint32_t i = 0; i < ROUNDS; ++i) {
            (void)alt.priSelect();
            out << v;
        }
    }
};

class SlowConsumer : public csp::CSProcessStatic<160> {
    csp::Chanin<uint32_t> in;

public:
    explicit SlowConsumer(csp::Chanin<uint32_t> r) : in(r) {}
    const char* name() const override { return "Slow consumer"; }
    void run() override {
        uint32_t v;
        for (uint32_t i = 0; i < ROUNDS; ++i) {
            in >> v;
            csp::SleepFor(csp::Ticks(1));
        }
    }
};

} // namespace

extern "C" void csp4cmsis_trace_throttled_drop_isr(void) {
    if (throttled_active) {
        const uint32_t sample = throttled_sample + 1u;
        throttled_sample = sample;
        (void)throttled_samples.isrWriter().putFromISR(sample);
    }
}

extern "C" void csp4cmsis_trace_throttled_drop(void) {
    size_t off = 0;
    ThrottledSampler& sampler = trace_pool_new<ThrottledSampler>(off, throttled_samples.reader(), throttled_forward.writer());
    SlowConsumer& consumer = trace_pool_new<SlowConsumer>(off, throttled_forward.reader());
    throttled_active = true;
    csp4cmsis_trace_env_timer_rate(RATE_HZ);
    csp4cmsis_trace_env_timer(1);
    csp::Run(csp::InParallel(sampler, consumer), csp::ExecutionMode::TerminatingNetwork);
    throttled_active = false;
    csp4cmsis_trace_env_timer(0);     // back to its default rate
    csp4cmsis_trace_pool_release();
}
#endif
