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
// csp4cmsis_trace_rendezvous_order(then) is called once after
// osKernelInitialize() and before osKernelStart(). It creates a thread that
// runs the two pairs as a TerminatingNetwork, then calls then() (if not null)
// and exits.
#if defined(CSP4CMSIS_TRACE)
#include "cmsis_os2.h"
#include "csp/csp4cmsis.h"

namespace {

constexpr uint32_t ROUNDS = 10u;
constexpr uint32_t PAUSE_TICKS = 2u;

class Writer : public csp::CSProcessStatic<256> {
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

class Reader : public csp::CSProcessStatic<256> {
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

void (*g_then)(void);

void order_thread(void*) {
    static csp::Channel<uint32_t> writer_first;
    static csp::Channel<uint32_t> reader_first;
    static Writer wf_writer(writer_first.writer(), "WF writer", 0u);
    static Reader wf_reader(writer_first.reader(), "WF reader", PAUSE_TICKS);
    static Writer rf_writer(reader_first.writer(), "RF writer", PAUSE_TICKS);
    static Reader rf_reader(reader_first.reader(), "RF reader", 0u);
    csp::Run(csp::InParallel(wf_writer, wf_reader, rf_writer, rf_reader),
             csp::ExecutionMode::TerminatingNetwork);
    if (g_then) g_then();
    osThreadExit();
}

#if defined(CSP4CMSIS_STATIC_ALLOCATION)
alignas(8) uint32_t order_stack[256];
csp::internal::csp_static_thread_storage_t order_tcb;
#endif

} // namespace

extern "C" void csp4cmsis_trace_rendezvous_order(void (*then)(void)) {
    g_then = then;
    osThreadAttr_t a = {};
    a.name = "rendezvous_order";
    a.priority = osPriorityHigh;
    a.stack_size = sizeof(uint32_t) * 256u;
#if defined(CSP4CMSIS_STATIC_ALLOCATION)
    a.stack_mem = order_stack;
    a.cb_mem = &order_tcb;
    a.cb_size = sizeof(order_tcb);
#endif
    (void)osThreadNew(order_thread, nullptr, &a);
}
#endif
