// Trace test: one record per put, by buffer policy (builds with CSP4CMSIS_TRACE
// only; without it this file is empty).
//
// csp4cmsis_trace_drop_policy() writes 1..5 into a KeepOldest and a KeepNewest
// channel of 2 elements that nobody reads. Expected CSP4CMSIS events on each
// channel (trace/README.md):
//   keep_oldest: 2 BufferedPut, 3 DroppedNew
//   keep_newest: 2 BufferedPut, 3 OverwroteOldest
// check_drop_policy.py checks them in the CSV of tools/trace/decode.py.
//
// Call it once, from a thread or after osKernelInitialize() and before
// osKernelStart() (its events then show the context pre-kernel), with
// component 0x50 enabled in Event Recorder.
#if defined(CSP4CMSIS_TRACE)
#include "csp/csp4cmsis.h"

extern "C" void csp4cmsis_trace_drop_policy(void) {
    static csp::BufferedChannel<uint32_t, 2, csp::BufferPolicy::KeepOldest> keep_oldest;
    static csp::BufferedChannel<uint32_t, 2, csp::BufferPolicy::KeepNewest> keep_newest;
    csp::Chanout<uint32_t> o = keep_oldest.writer(), n = keep_newest.writer();
    for (uint32_t i = 1; i <= 5; ++i) { o << i; n << i; }
}
#endif
