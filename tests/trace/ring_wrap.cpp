// Trace test: ring slots of a buffered channel that wraps around (builds with
// CSP4CMSIS_TRACE only; without it this file is empty).
//
// csp4cmsis_trace_ring_wrap() puts to and gets from a Block channel of 3
// elements in the order of OPS (P put, G get; never more than 3 elements, so
// nothing blocks), from the calling thread. The ring wraps around twice.
// Expected events (trace/README.md): one BufferedPut or BufferedGet per
// letter, in that order, with the slots of a ring of 3 that starts at slot 0:
//   P0 P1 G0 P2 P0 G1 G2 P1 P2 G0 G1 G2 P0 G0
// check_ring_slots.py checks them, and drop_policy.cpp's, in the CSV of
// tools/trace/decode.py.
#if defined(CSP4CMSIS_TRACE)
#include "csp/csp4cmsis.h"

extern "C" const char csp4cmsis_trace_ring_wrap_ops[] = "PPGPPGGPPGGGPG";

extern "C" void csp4cmsis_trace_ring_wrap(void) {
    static csp::BufferedChannel<uint32_t, 3> ring_wrap;
    csp::Chanout<uint32_t> out = ring_wrap.writer();
    csp::Chanin<uint32_t> in = ring_wrap.reader();
    uint32_t next = 1u;
    uint32_t v;
    for (const char* op = csp4cmsis_trace_ring_wrap_ops; *op != '\0'; ++op) {
        if (*op == 'P') out << next++;
        else in >> v;
    }
}
#endif
