// Trace test: the offers of ALT waits (builds with CSP4CMSIS_TRACE only;
// without it this file is empty).
//
// csp4cmsis_trace_alt_offers() runs two processes. "ALT offers" waits once in
// each of three Alternatives:
//   alt_timeout: guard 0 input alt_idle (nobody writes), guard 1 timeout
//                3 ticks; ends by its timeout (guard 1);
//   alt_output:  guard 0 output alt_out (rendezvous), guard 1 input alt_idle;
//                "ALT partner" reads alt_out after 2 ticks (guard 0);
//   alt_full:    guard 0 output alt_full_buf (Block, 1 element, full),
//                guard 1 timeout 2 ticks; ends by its timeout (guard 1).
// The API has no guard preconditions, so a waiting select() has every guard
// enabled. Expected events (trace/README.md), per Alternative: AltGuard for
// guards 0 and 1 (kind and target as above), exactly one WaitAlt with
// enabled mask 0x3 and 2 guards, and AltSelected with the guard above.
// check_alt_offers.py checks them, and T12's (suite), in the CSV of
// tools/trace/decode.py.
#if defined(CSP4CMSIS_TRACE)
#include "csp/csp4cmsis.h"
#include "trace_pool.h"

namespace {

csp::Channel<uint32_t> alt_idle;
csp::Channel<uint32_t> alt_out;
csp::BufferedChannel<uint32_t, 1> alt_full_buf;

class AltOffers : public csp::CSProcessStatic<256> {   // select() with Event Recorder at -O0: more than 640 B
    csp::Chanin<uint32_t> idle;
    csp::Chanin<uint32_t> idle2;
    csp::Chanout<uint32_t> out;
    csp::Chanout<uint32_t> full_out;
    csp::Chanin<uint32_t> full_in;

public:
    AltOffers(csp::Chanin<uint32_t> i, csp::Chanin<uint32_t> i2, csp::Chanout<uint32_t> o, csp::Chanout<uint32_t> fo,
              csp::Chanin<uint32_t> fi)
        : idle(i), idle2(i2), out(o), full_out(fo), full_in(fi) {}
    const char* name() const override { return "ALT offers"; }
    void run() override {
        static uint32_t in_value;
        static const uint32_t out_value = 7u;
        static csp::RelTimeoutGuard after3(csp::Ticks(3));
        static csp::RelTimeoutGuard after2(csp::Ticks(2));
        static csp::Alternative alt_timeout(idle | in_value, after3);
        static csp::Alternative alt_output(out | out_value, idle2 | in_value);
        static csp::Alternative alt_full(full_out | out_value, after2);
        (void)alt_timeout.priSelect();
        (void)alt_output.priSelect();
        full_out << out_value;
        (void)alt_full.priSelect();
        full_in >> in_value;
    }
};

class AltPartner : public csp::CSProcessStatic<160> {
    csp::Chanin<uint32_t> in;

public:
    explicit AltPartner(csp::Chanin<uint32_t> r) : in(r) {}
    const char* name() const override { return "ALT partner"; }
    void run() override {
        uint32_t v;
        csp::SleepFor(csp::Ticks(5));
        in >> v;
    }
};

} // namespace

extern "C" void csp4cmsis_trace_alt_offers(void) {
    size_t off = 0;
    AltOffers& offers = trace_pool_new<AltOffers>(off, alt_idle.reader(), alt_idle.reader(), alt_out.writer(), alt_full_buf.writer(),
                            alt_full_buf.reader());
    AltPartner& partner = trace_pool_new<AltPartner>(off, alt_out.reader());
    csp::Run(csp::InParallel(offers, partner), csp::ExecutionMode::TerminatingNetwork);
    csp4cmsis_trace_pool_release();
}
#endif
