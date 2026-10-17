// Trace demo: a small named network for the Perfetto view (builds with
// CSP4CMSIS_TRACE only; without it this file is empty).
//
//   ProducerA --a_to_merger--.
//                             >-- Merger --merger_to_consumer--> Consumer
//   ProducerB --b_to_merger--'      ^
//   interrupt --sensor_samples------'   (KeepNewest, 2 elements, putFromISR)
//
// ProducerA and ProducerB send DEMO_ROUNDS values each on their rendezvous
// channel, A every tick, B every third tick. Merger selects fairly among
// a_to_merger, b_to_merger, sensor_samples and a 2-tick timeout, forwards
// the producers' values on merger_to_consumer and keeps the latest sensor
// sample; Consumer reads 2 x DEMO_ROUNDS values. Then the network ends.
//
// csp4cmsis_trace_demo_network(): runs the network to its end (Run,
// TerminatingNetwork); call it from a thread.
// csp4cmsis_trace_demo_sensor_isr(): call it from a periodic interrupt handler
// while the network runs (priority numerically >= CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY).
#if defined(CSP4CMSIS_TRACE)
#include "csp/csp4cmsis.h"

#define DEMO_ROUNDS 20U

static csp::Channel<uint32_t> a_to_merger;
static csp::Channel<uint32_t> b_to_merger;
static csp::Channel<uint32_t> merger_to_consumer;
static csp::BufferedChannel<uint32_t, 2, csp::BufferPolicy::KeepNewest> sensor_samples;

namespace {

class Producer : public csp::CSProcessStatic<256> {
public:
    Producer(const char* name, csp::Channel<uint32_t>& out, uint32_t period)
        : name_(name), out_(out), period_(period) {}
    const char* name() const override { return name_; }
protected:
    void run() override {
        csp::Chanout<uint32_t> out = out_.writer();
        for (uint32_t i = 1; i <= DEMO_ROUNDS; ++i) {
            out << i;
            csp::SleepFor(csp::Ticks(period_));
        }
    }
private:
    const char* name_;
    csp::Channel<uint32_t>& out_;
    uint32_t period_;
};

class Merger : public csp::CSProcessStatic<256> {
public:
    const char* name() const override { return "Merger"; }
    volatile uint32_t last_sample = 0;
protected:
    void run() override {
        csp::Chanin<uint32_t> a = a_to_merger.reader(), b = b_to_merger.reader();
        csp::Chanin<uint32_t> sensor = sensor_samples.reader();
        csp::Chanout<uint32_t> out = merger_to_consumer.writer();
        uint32_t va = 0, vb = 0, vs = 0;
        csp::RelTimeoutGuard quiet(csp::Ticks(2));
        csp::Alternative alt(a | va, b | vb, sensor | vs, quiet);
        for (uint32_t forwarded = 0; forwarded < 2U * DEMO_ROUNDS;) {
            switch (alt.fairSelect()) {
            case 0: out << va; ++forwarded; break;
            case 1: out << vb; ++forwarded; break;
            case 2: last_sample = vs; break;
            default: break;                         // quiet for 2 ticks
            }
        }
    }
};

class Consumer : public csp::CSProcessStatic<256> {
public:
    const char* name() const override { return "Consumer"; }
    volatile uint32_t sum = 0;
protected:
    void run() override {
        csp::Chanin<uint32_t> in = merger_to_consumer.reader();
        for (uint32_t i = 0; i < 2U * DEMO_ROUNDS; ++i) { uint32_t v; in >> v; sum = sum + v; }
    }
};

Producer producer_a("ProducerA", a_to_merger, 1);
Producer producer_b("ProducerB", b_to_merger, 3);
Merger merger;
Consumer consumer;

} // namespace

extern "C" void csp4cmsis_trace_demo_network(void) {
    csp::Run(csp::InParallel(producer_a, producer_b, merger, consumer),
             csp::ExecutionMode::TerminatingNetwork);
}

extern "C" void csp4cmsis_trace_demo_sensor_isr(void) {
    static uint32_t sample;
    sample = sample + 1U;
    (void)sensor_samples.isrWriter().putFromISR(sample);
}
#endif
