// Trace tests that need threads (builds with CSP4CMSIS_TRACE only; without
// it this file is empty).
//
// csp4cmsis_trace_tests_start(then) is called once after osKernelInitialize()
// and before osKernelStart(). It creates a thread that runs, one after the
// other,
//   rendezvous_order.cpp  plain rendezvous in both arrival orders
//   ring_wrap.cpp         ring slots of a buffer that wraps around
//   alt_offers.cpp        WaitAlt guard masks and AltGuard descriptions
//   stable_env.cpp        a stable point awaiting an interrupt
// then calls then() (if not null) and exits. stable_env.cpp needs a periodic
// interrupt from the platform: see there.
#if defined(CSP4CMSIS_TRACE)
#include "cmsis_os2.h"
#include "csp/csp4cmsis.h"

extern "C" void csp4cmsis_trace_rendezvous_order(void);
extern "C" void csp4cmsis_trace_ring_wrap(void);
extern "C" void csp4cmsis_trace_alt_offers(void);
extern "C" void csp4cmsis_trace_stable_env(void);

namespace {

constexpr uint32_t STACK_WORDS = 384u;   // Run() with Event Recorder at -O0: more than 1 KB

void (*g_then)(void);

void tests_thread(void*) {
    csp4cmsis_trace_rendezvous_order();
    csp4cmsis_trace_ring_wrap();
    csp4cmsis_trace_alt_offers();
    csp4cmsis_trace_stable_env();
    if (g_then) g_then();
    osThreadExit();
}

#if defined(CSP4CMSIS_STATIC_ALLOCATION)
alignas(8) uint32_t tests_stack[STACK_WORDS];
csp::internal::csp_static_thread_storage_t tests_tcb;
#endif

} // namespace

extern "C" void csp4cmsis_trace_tests_start(void (*then)(void)) {
    g_then = then;
    osThreadAttr_t a = {};
    a.name = "trace_tests";
    a.priority = osPriorityHigh;
    a.stack_size = sizeof(uint32_t) * STACK_WORDS;
#if defined(CSP4CMSIS_STATIC_ALLOCATION)
    a.stack_mem = tests_stack;
    a.cb_mem = &tests_tcb;
    a.cb_size = sizeof(tests_tcb);
#endif
    (void)osThreadNew(tests_thread, nullptr, &a);
}
#endif
