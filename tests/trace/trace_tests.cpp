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
//   stable_multi.cpp      stable points of several processes awaiting a
//                         timeout and an interrupt
//   fair_select.cpp       fairSelect() with both guards ready
//   throttled_drop.cpp    a slow consumer makes an interrupt-fed KeepOldest
//                         channel drop samples during operation
// then calls then() (if not null) and exits. stable_env.cpp,
// stable_multi.cpp and throttled_drop.cpp need a periodic interrupt from the
// platform: see stable_env.cpp and throttled_drop.cpp.
#if defined(CSP4CMSIS_TRACE)
#include "cmsis_os2.h"
#include "csp/csp4cmsis.h"
#include "csp/csp_fatal.h"

extern "C" void csp4cmsis_trace_rendezvous_order(void);
extern "C" void csp4cmsis_trace_ring_wrap(void);
extern "C" void csp4cmsis_trace_alt_offers(void);
extern "C" void csp4cmsis_trace_stable_env(void);
extern "C" void csp4cmsis_trace_stable_multi(void);
extern "C" void csp4cmsis_trace_fair_select(void);
extern "C" void csp4cmsis_trace_throttled_drop(void);

namespace {

constexpr uint32_t STACK_WORDS = 384u;   // Run() with Event Recorder at -O0: more than 1 KB

void (*g_then)(void);

// The pool of trace_pool.h: the processes of one test at a time (the largest,
// rendezvous_order.cpp's four, need about 3.2 KB).
constexpr size_t POOL_BYTES = 3584u;
alignas(8) unsigned char g_pool[POOL_BYTES];

void tests_thread(void*) {
    csp4cmsis_trace_rendezvous_order();
    csp4cmsis_trace_ring_wrap();
    csp4cmsis_trace_alt_offers();
    csp4cmsis_trace_stable_env();
    csp4cmsis_trace_stable_multi();
    csp4cmsis_trace_fair_select();
    csp4cmsis_trace_throttled_drop();
    if (g_then) g_then();
    osThreadExit();
}

#if defined(CSP4CMSIS_STATIC_ALLOCATION)
alignas(8) uint32_t tests_stack[STACK_WORDS];
csp::internal::csp_static_thread_storage_t tests_tcb;
#endif

} // namespace

extern "C" void* csp4cmsis_trace_pool(size_t bytes) {
    if (bytes > POOL_BYTES) csp::internal::fatal("trace tests: process pool too small (trace_tests.cpp POOL_BYTES)");
    return g_pool;
}

extern "C" void csp4cmsis_trace_pool_release(void) {
    osDelay(3);     // the processes' threads exit and (FreeRTOS) the idle task deletes them
}

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
