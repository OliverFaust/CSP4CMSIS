// Trace test: fairSelect() with both guards ready (builds with
// CSP4CMSIS_TRACE only; without it this file is empty).
//
// csp4cmsis_trace_fair_select() runs "Fair writer a" and "Fair writer b",
// which send MESSAGES values each on the rendezvous channels fair_a and
// fair_b, and "Fair reader", which sleeps a tick before each of its
// 2 * MESSAGES fairSelect()s over (fair_a, fair_b): by then both writers
// have offered, so both guards are ready when each select() starts (after
// the sleep the reader puts and gets one value on fair_mark, its own
// buffered channel: records that show where each select() starts). The
// suite's T12 has no such moment (its reader takes each offer at once).
// Expected (trace/README.md): fairSelect() alternates; check_fair_select.py
// finds every select (but the first and the last few) with the other guard
// ready throughout, no violation of fairSelect()'s rule, and violations of
// priSelect()'s.
#if defined(CSP4CMSIS_TRACE)
#include "csp/csp4cmsis.h"
#include "trace_pool.h"

namespace {

constexpr uint32_t MESSAGES = 8u;

csp::Channel<uint32_t> fair_a;
csp::Channel<uint32_t> fair_b;
csp::BufferedChannel<uint32_t, 1> fair_mark;

class FairWriter : public csp::CSProcessStatic<160> {
    csp::Chanout<uint32_t> out;
    const char* nm;

public:
    FairWriter(csp::Chanout<uint32_t> o, const char* n) : out(o), nm(n) {}
    const char* name() const override { return nm; }
    void run() override {
        for (uint32_t i = 0; i < MESSAGES; ++i) out << i;
    }
};

class FairReader : public csp::CSProcessStatic<224> {   // select() with Event Recorder at -O0: more than 640 B
    csp::Chanin<uint32_t> a;
    csp::Chanin<uint32_t> b;
    csp::Chanout<uint32_t> mark_out;
    csp::Chanin<uint32_t> mark_in;

public:
    FairReader(csp::Chanin<uint32_t> x, csp::Chanin<uint32_t> y, csp::Chanout<uint32_t> mo, csp::Chanin<uint32_t> mi)
        : a(x), b(y), mark_out(mo), mark_in(mi) {}
    const char* name() const override { return "Fair reader"; }
    void run() override {
        uint32_t va = 0, vb = 0, m = 0;
        csp::Alternative alt(a | va, b | vb);
        for (uint32_t i = 0; i < 2u * MESSAGES; ++i) {
            csp::SleepFor(csp::Ticks(1));
            mark_out << i;
            mark_in >> m;
            (void)alt.fairSelect();
        }
    }
};

} // namespace

extern "C" void csp4cmsis_trace_fair_select(void) {
    size_t off = 0;
    FairWriter& wa = trace_pool_new<FairWriter>(off, fair_a.writer(), "Fair writer a");
    FairWriter& wb = trace_pool_new<FairWriter>(off, fair_b.writer(), "Fair writer b");
    FairReader& reader = trace_pool_new<FairReader>(off, fair_a.reader(), fair_b.reader(), fair_mark.writer(), fair_mark.reader());
    csp::Run(csp::InParallel(wa, wb, reader), csp::ExecutionMode::TerminatingNetwork);
    csp4cmsis_trace_pool_release();
}
#endif
