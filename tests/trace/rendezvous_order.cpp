// Trace test: one RendezvousDone per plain-writer/plain-reader communication,
// in both arrival orders (builds with CSP4CMSIS_TRACE only; without it this
// file is empty).
//
// Two writer/reader pairs, ROUNDS values each, plain ends on both sides:
//   writer_first: the reader sleeps before each read, so the writer arrives
//                 first and the reader completes (by_reader=1);
//   reader_first: the writer sleeps before each write, so the reader arrives
//                 first and the writer completes (by_reader=0).
// Expected CSP4CMSIS events (trace/README.md): exactly ROUNDS RendezvousDone
// on each channel, all with by_reader=1 on writer_first and by_reader=0 on
// reader_first, writer and reader the pair's two threads.
// check_rendezvous_order.py checks them in the CSV of tools/trace/decode.py.
//
// csp4cmsis_trace_rendezvous_order() runs the two pairs as a
// TerminatingNetwork; call it from a thread (trace_tests.cpp).
#if defined(CSP4CMSIS_TRACE)
#include "csp/csp4cmsis.h"
#include "trace_pool.h"

namespace {

constexpr uint32_t ROUNDS = 10u;
constexpr uint32_t PAUSE_TICKS = 2u;

class Writer : public csp::CSProcessStatic<160> {
    csp::Chanout<uint32_t> out;
    const char* label;
    uint32_t pause;

public:
    Writer(csp::Chanout<uint32_t> w, const char* process_name, uint32_t pause_ticks)
        : out(w), label(process_name), pause(pause_ticks) {}
    const char* name() const override { return label; }
    void run() override {
        for (uint32_t i = 1; i <= ROUNDS; ++i) {
            if (pause) csp::SleepFor(csp::Ticks(pause));
            out << i;
        }
    }
};

class Reader : public csp::CSProcessStatic<160> {
    csp::Chanin<uint32_t> in;
    const char* label;
    uint32_t pause;

public:
    Reader(csp::Chanin<uint32_t> r, const char* process_name, uint32_t pause_ticks)
        : in(r), label(process_name), pause(pause_ticks) {}
    const char* name() const override { return label; }
    void run() override {
        uint32_t v;
        for (uint32_t i = 1; i <= ROUNDS; ++i) {
            if (pause) csp::SleepFor(csp::Ticks(pause));
            in >> v;
        }
    }
};

} // namespace

extern "C" void csp4cmsis_trace_rendezvous_order(void) {
    static csp::Channel<uint32_t> writer_first;
    static csp::Channel<uint32_t> reader_first;
    size_t off = 0;
    Writer& wf_writer = trace_pool_new<Writer>(off, writer_first.writer(), "WF writer", 0u);
    Reader& wf_reader = trace_pool_new<Reader>(off, writer_first.reader(), "WF reader", PAUSE_TICKS);
    Writer& rf_writer = trace_pool_new<Writer>(off, reader_first.writer(), "RF writer", PAUSE_TICKS);
    Reader& rf_reader = trace_pool_new<Reader>(off, reader_first.reader(), "RF reader", 0u);
    csp::Run(csp::InParallel(wf_writer, wf_reader, rf_writer, rf_reader),
             csp::ExecutionMode::TerminatingNetwork);
    csp4cmsis_trace_pool_release();
}
#endif
