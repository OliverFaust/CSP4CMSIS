// =============================================================================
// CSP4CMSIS BufferedChannel / ALT regression suite (Corstone-300 FVP)
//
// Runs unchanged against
//   * the v1.0.0 library  (internal BufferedChannel<T, P>(capacity), osMessageQueue)
//   * the v2 library      (internal BufferedChannel<T, SIZE, P>, ring buffer)
// and against both CMSIS-RTOS2 backends of the harness project
//   * FreeRTOS 11.3.0 via ARM::CMSIS-FreeRTOS    (build type .FreeRTOS)
//   * Keil RTX5 5.9.1 via ARM::CMSIS-RTX         (build type .RTX5)
//
// Every test prints   RESULT <id>: PASS | FAIL | SKIP -- <what is checked>
// FAIL means the defect is present. SKIP means the test needs a v2-only hook.
// The run ends with a SUMMARY line and EOT (0x04), which stops the FVP.
//
// Test code, not library code: it uses csp::internal classes on purpose.
// Timing: the FVP tick runs at ~312.5 Hz on both backends; only tick counts
// are used. See README.md for the method and BUFFERED_CHANNEL_ANALYSIS.md.
//
// Other targets (MPS2 Cortex-M4 FVP, Alif DK-E8) set the BC_* platform macros
// below; their defaults are the Corstone-300 FVP values.
// =============================================================================
#include "csp/csp4cmsis.h"
#include "cmsis_os2.h"
// Device header and backend: pack builds get them from RTE_Components.h;
// builds without packs (STM32CubeIDE) from CSP4CMSIS_DEVICE_HEADER and the
// CSP4CMSIS_RTOS2_BACKEND_* define, as the library itself does.
#if __has_include("RTE_Components.h")
#include "RTE_Components.h"
#endif
#if defined(CMSIS_device_header)
#include CMSIS_device_header
#else
#include CSP4CMSIS_DEVICE_HEADER
#endif
#if defined(RTE_CMSIS_RTOS2_FreeRTOS) || (!defined(RTE_CMSIS_RTOS2_RTX5) && defined(CSP4CMSIS_RTOS2_BACKEND_FREERTOS))
#define BC_FREERTOS 1
#elif defined(RTE_CMSIS_RTOS2_RTX5) || defined(CSP4CMSIS_RTOS2_BACKEND_RTX5)
#define BC_RTX5 1
#endif
#include <cstdio>
#include <cstdint>
#include <cstring>

// ---- platform parameters (defaults: Corstone-300 FVP) ------------------------
// Software-interrupt source for the ISR-side operations (T2, T3i, T15i, T16s,
// T16a). The handler is linked into the vector table at build time, so this
// also works where the vector table is in read-only memory.
#ifndef BC_SWI_IRQn
#define BC_SWI_IRQn     I2S_IRQn
#define BC_SWI_HANDLER  I2S_Handler
#endif
// NVIC priority of that interrupt, as passed to NVIC_SetPriority(). Must be
// numerically >= CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY (masked by CSP
// critical sections). Corstone-300 and MPS2 Cortex-M4: 6 of 0..7 (3 bits);
// DK-E8: 192 of 0..255 (8 bits).
#ifndef BC_SWI_PRIO
#define BC_SWI_PRIO     6
#endif
// Phase sweeps: binary-search upper bound for kb (spin iterations; must exceed
// the iterations per tick), and the swept range kb-BELOW .. kb+ABOVE.
#ifndef BC_SEARCH_HI
#define BC_SEARCH_HI    400000u
#endif
#ifndef BC_SWEEP_BELOW
#define BC_SWEEP_BELOW  1500u
#endif
#ifndef BC_SWEEP_ABOVE
#define BC_SWEEP_ABOVE  1000u
#endif

#if defined(BC_FREERTOS)
  #include "FreeRTOS.h"
  #ifdef BC_BACKEND_NAME
  #define BACKEND_NAME BC_BACKEND_NAME
  #else
  #define BACKEND_NAME "FreeRTOS 11.3.0 (CMSIS-RTOS2 adapter)"
  #endif
  #if (configSUPPORT_DYNAMIC_ALLOCATION == 0)
    // Heap-free build: no dynamic-allocation API exists; not touching the
    // heap keeps heap_4 (pvPortMalloc) out of the linked image.
    #define HEAP_MODE "RTOS dynamic allocation DISABLED (configSUPPORT_DYNAMIC_ALLOCATION=0)"
    static uint32_t heap_used() { return 0; }
  #else
    #define HEAP_MODE "RTOS heap enabled"
    static uint32_t heap_used() { return (uint32_t)(configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize()); }
  #endif
#elif defined(BC_RTX5)
  #include "rtx_os.h"
  #define BACKEND_NAME "Keil RTX5 5.9.1"
  // RTX5 dynamic memory pool: mem_head_t { uint32_t size; uint32_t used; } (rtx_memory.c).
  // Heap-free build (OS_DYNAMIC_MEM_SIZE=0): no pool at all (mem.common == NULL),
  // so every dynamic creation would return NULL.
  #define HEAP_MODE (osRtxInfo.mem.common == nullptr ? "RTOS dynamic allocation DISABLED (RTX5: no dynamic memory pool)" : "RTOS heap enabled")
  static uint32_t heap_used() {
      return osRtxInfo.mem.common == nullptr ? 0U : static_cast<const uint32_t*>(osRtxInfo.mem.common)[1];
  }
  extern "C" uint32_t osRtxErrorNotify(uint32_t code, void* object_id) {
      printf("!! RTX5 osRtxErrorNotify(code=%lu, object=%p) -- halting\r\n", (unsigned long)code, object_id);
      for (;;) { }
  }
#else
  #error "unknown CMSIS-RTOS2 backend"
#endif

// ---------------------------------------------------------------------------
// Library-version shim. 3.0 on: csp/csp_version.h (no feature macros). Before:
// the feature macros of 2.x (undefined in 1.x).
// ---------------------------------------------------------------------------
#if __has_include("csp/csp_version.h")
  #define BC_LIB3          1
  #define BC_ISR_WRITER    1
  #define BC_OWRV          1
  #define BC_BUFFERED_V2   1
  #define BC_SLEEPFOR_TIME 1
#else
  #define BC_LIB3          0
  #if defined(CSP4CMSIS_ISR_WRITER_API)
    #define BC_ISR_WRITER  1
  #else
    #define BC_ISR_WRITER  0
  #endif
  #if defined(CSP4CMSIS_ALT_PROTOCOL_OWRV)
    #define BC_OWRV        1
  #else
    #define BC_OWRV        0
  #endif
  #if defined(CSP4CMSIS_BUFFERED_CHANNEL_API) && (CSP4CMSIS_BUFFERED_CHANNEL_API >= 2)
    #define BC_BUFFERED_V2 1
  #else
    #define BC_BUFFERED_V2 0
  #endif
  #if defined(CSP4CMSIS_SLEEPFOR_TIME_API)
    #define BC_SLEEPFOR_TIME 1
  #else
    #define BC_SLEEPFOR_TIME 0
  #endif
#endif
// ALT construction. 3.0: Alternative alt(in | v, timeout) (guard internals are
// private). 1.x/2.x: Alternative alt({in.getGuard(v), timeout.internal_guard_ptr}).
// Usage: csp::Alternative alt BC_ALTV(BC_G(in, v), BC_T(timeout));
#if BC_LIB3
  #define BC_ALTV(...)     (__VA_ARGS__)
  #define BC_G(end, var)   (end) | (var)
  #define BC_T(tg)         (tg)
  #define BC_OUT_GUARD(out, var) csp::internal::Access::outputGuard(out, var)
  using BC_SignalChannel = csp::SignalChannel;
#else
  #define BC_ALTV(...)     ({__VA_ARGS__})
  #define BC_G(end, var)   (end).getGuard(var)
  #define BC_T(tg)         (tg).internal_guard_ptr
  #define BC_OUT_GUARD(out, var) (out).getGuard(var)
#endif
#if !BC_LIB3
  using BC_SignalChannel = csp::SignalChannel<>;
#endif

#if BC_BUFFERED_V2
  #define LIB_V2 1
  #define LIB_NAME "v2 API (ring buffer)"
  template <typename T, size_t N, csp::BufferPolicy P>
  using BChan = csp::internal::BufferedChannel<T, N, P>;
#else
  #define LIB_V2 0
  #define LIB_NAME "v1 API (osMessageQueue)"
  template <typename T, size_t N, csp::BufferPolicy P>
  struct BChan : csp::internal::BufferedChannel<T, P> {
      BChan() : csp::internal::BufferedChannel<T, P>(N) {}
  };
#endif

using csp::BufferPolicy;
template <size_t N> using BlockChan  = BChan<uint32_t, N, BufferPolicy::Block>;
template <size_t N> using NewestChan = BChan<uint32_t, N, BufferPolicy::KeepNewest>;
using In  = csp::Chanin<uint32_t>;
using Out = csp::Chanout<uint32_t>;

// v2 fatal-error hook (weak in the library). The test's version records the
// message and parks the calling thread, so the runner can check it.
static volatile uint32_t    g_fatal_count = 0;
static const char* volatile g_fatal_msg   = nullptr;
extern "C" void csp4cmsis_fatal_error(const char* msg) {
    g_fatal_msg = msg; g_fatal_count = g_fatal_count + 1;
    for (;;) osDelay(osWaitForever);
}

// ---------------------------------------------------------------------------
// RTOS calls made with BASEPRI raised (i.e. inside a CSP critical section).
// armlink $Sub$$/$Super$$ patching (Arm Compiler) or GNU ld --wrap (GCC)
// intercepts every call from other objects (the library) to these
// CMSIS-RTOS2 functions on both backends.
// ---------------------------------------------------------------------------
static volatile uint32_t g_rtos_in_crit = 0;
static const char* volatile g_rtos_in_crit_fn = nullptr;
static inline void crit_check(const char* fn) {
    if (__get_BASEPRI() != 0U) { g_rtos_in_crit = g_rtos_in_crit + 1; g_rtos_in_crit_fn = fn; }
}
extern "C" {
#if defined(__ARMCC_VERSION)          // armlink: $Sub$$name replaces name, $Super$$name is the original
#define WRAP(ret, name, params, args)                                  \
    ret $Super$$##name params;                                         \
    ret $Sub$$##name params { crit_check(#name); return $Super$$##name args; }
#else                                 // GNU ld: -Wl,--wrap=name (see the harness cproject)
#define WRAP(ret, name, params, args)                                  \
    ret __real_##name params;                                          \
    ret __wrap_##name params { crit_check(#name); return __real_##name args; }
#endif
WRAP(uint32_t,   osEventFlagsSet,        (osEventFlagsId_t e, uint32_t f), (e, f))
WRAP(uint32_t,   osThreadFlagsSet,       (osThreadId_t t, uint32_t f), (t, f))
WRAP(osStatus_t, osSemaphoreRelease,     (osSemaphoreId_t s), (s))
WRAP(osStatus_t, osSemaphoreAcquire,     (osSemaphoreId_t s, uint32_t t), (s, t))
WRAP(osStatus_t, osMessageQueuePut,      (osMessageQueueId_t q, const void* m, uint8_t p, uint32_t t), (q, m, p, t))
WRAP(osStatus_t, osMessageQueueGet,      (osMessageQueueId_t q, void* m, uint8_t* p, uint32_t t), (q, m, p, t))
WRAP(uint32_t,   osMessageQueueGetCount, (osMessageQueueId_t q), (q))
WRAP(uint32_t,   osMessageQueueGetSpace, (osMessageQueueId_t q), (q))
WRAP(osStatus_t, osMutexAcquire,         (osMutexId_t m, uint32_t t), (m, t))
WRAP(osStatus_t, osMutexRelease,         (osMutexId_t m), (m))
#undef WRAP
// T6: RTOS timers created during the test (2.0.1: a timeout guard is none).
static volatile uint32_t g_timer_news = 0;
#if defined(__ARMCC_VERSION)
osTimerId_t $Super$$osTimerNew(osTimerFunc_t f, osTimerType_t t, void* a, const osTimerAttr_t* at);
osTimerId_t $Sub$$osTimerNew(osTimerFunc_t f, osTimerType_t t, void* a, const osTimerAttr_t* at) {
    g_timer_news = g_timer_news + 1; return $Super$$osTimerNew(f, t, a, at);
}
#else
osTimerId_t __real_osTimerNew(osTimerFunc_t f, osTimerType_t t, void* a, const osTimerAttr_t* at);
osTimerId_t __wrap_osTimerNew(osTimerFunc_t f, osTimerType_t t, void* a, const osTimerAttr_t* at) {
    g_timer_news = g_timer_news + 1; return __real_osTimerNew(f, t, a, at);
}
#endif
// T28: make the next osThreadNew() call fail (returns NULL without creating a thread).
static volatile uint32_t g_fail_next_thread_new = 0;
#if defined(__ARMCC_VERSION)
osThreadId_t $Super$$osThreadNew(osThreadFunc_t f, void* a, const osThreadAttr_t* at);
osThreadId_t $Sub$$osThreadNew(osThreadFunc_t f, void* a, const osThreadAttr_t* at) {
    if (g_fail_next_thread_new) { g_fail_next_thread_new = 0; return nullptr; }
    return $Super$$osThreadNew(f, a, at);
}
#else
osThreadId_t __real_osThreadNew(osThreadFunc_t f, void* a, const osThreadAttr_t* at);
osThreadId_t __wrap_osThreadNew(osThreadFunc_t f, void* a, const osThreadAttr_t* at) {
    if (g_fail_next_thread_new) { g_fail_next_thread_new = 0; return nullptr; }
    return __real_osThreadNew(f, a, at);
}
#endif
}

// ---------------------------------------------------------------------------
// Infrastructure
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t F_START = 0x10000000u;   // runner -> victim (outside CSP4CMSIS's reserved bits)
constexpr uint32_t F_ACK   = 0x20000000u;   // victim -> runner
constexpr uint32_t T_ACK   = 30u;           // ticks before a victim counts as hung

// Test threads: static stacks; static control blocks unless the library is
// built with dynamic allocation (3.0: CSP4CMSIS_DYNAMIC_ALLOCATION), where the
// backend's control-block types are not declared.
template <size_t WORDS>
struct ThreadSlotN {
#if defined(CSP4CMSIS_STATIC_ALLOCATION)
    alignas(8) uint32_t stack[WORDS];
    csp::internal::csp_static_thread_storage_t tcb;
#endif
};
using ThreadSlot = ThreadSlotN<256>;      // 1 KB workers (T5 needs a 32 KB static channel in RAM)
using RunnerSlot = ThreadSlotN<2048>;     // 8 KB runner

template <size_t WORDS>
osThreadId_t spawn(osThreadFunc_t fn, void* arg, osPriority_t prio, ThreadSlotN<WORDS>& s, const char* name) {
    osThreadAttr_t a = {};
    a.name = name; a.priority = prio;
    a.stack_size = WORDS * sizeof(uint32_t);
#if defined(CSP4CMSIS_STATIC_ALLOCATION)
    a.stack_mem = s.stack;
    a.cb_mem = &s.tcb;     a.cb_size = sizeof(s.tcb);
#else
    (void)s;                 // dynamic: the RTOS allocates stack and control block together
#endif
    return osThreadNew(fn, arg, &a);
}

void spin(uint32_t k) { for (volatile uint32_t i = 0; i < k; ++i) { } }
void park() { for (;;) osDelay(osWaitForever); }
template <typename C> void drain(C& ch) { uint32_t x; while (ch.pending()) ch.input(&x); }
// ISR write: 2.0 through the public ISR writer end (IsrChanout), 1.x through
// the channel's own putFromISR().
#if BC_ISR_WRITER
template <typename C> bool isr_put(C* ch, uint32_t v) { csp::IsrChanout<uint32_t> w(ch); return w.putFromISR(v); }
#else
template <typename C> bool isr_put(C* ch, uint32_t v) { return ch->putFromISR(v); }
#endif

uint32_t g_pass = 0, g_fail = 0, g_skip = 0, g_replaced = 0;
// A test whose defect became impossible to write: the API that allowed it was
// removed, and a compile check (tests/compile_checks/) proves the removal.
void replaced(const char* id, const char* check, const char* what) {
    g_replaced++;
    printf("RESULT %s: REPLACED -- %s (compile check %s)\r\n", id, what, check);
}
void result(const char* id, int verdict /*1 pass, 0 fail, -1 skip*/, const char* what) {
    const char* v = verdict > 0 ? "PASS" : (verdict == 0 ? "FAIL" : "SKIP");
    if (verdict > 0) g_pass++; else if (verdict == 0) g_fail++; else g_skip++;
    printf("RESULT %s: %s -- %s\r\n", id, v, what);
}

// ---------------------------------------------------------------------------
// Software interrupt BC_SWI_IRQn (Corstone-300: I2S_IRQn, unused): the
// aggressor pends it and the handler runs the ISR-side operation of the
// current test. Priority BC_SWI_PRIO is below
// CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY, i.e. masked by CSP critical
// sections, as required for putFromISR().
// ---------------------------------------------------------------------------
void (* volatile g_isr_op)() = nullptr;
void isr_init() { NVIC_SetPriority(BC_SWI_IRQn, BC_SWI_PRIO); NVIC_ClearPendingIRQ(BC_SWI_IRQn); NVIC_EnableIRQ(BC_SWI_IRQn); }
void isr_fire() { NVIC_SetPendingIRQ(BC_SWI_IRQn); __DSB(); __ISB(); }
} // namespace
extern "C" void BC_SWI_HANDLER(void) { if (g_isr_op) g_isr_op(); }
namespace {

// ---------------------------------------------------------------------------
// Phase-sweep harness (see README.md). Runner = higher-priority aggressor.
// ---------------------------------------------------------------------------
enum Outcome { EARLY = 0, LATE = 1, BUG = 2, ANOMALY = 3 };

struct Case {
    const char* id;
    void (*reset)();
    bool (*probe)();
    void (*victim_op)();
    void (*aggressor_op)();
    bool (*stuck)();
    void (*rescue)();
    bool (*check)();
    osThreadId_t victim;
    osThreadId_t runner;
};

volatile uint32_t g_k = 0, g_stage = 0, g_seen = 0;
volatile bool     g_pre = false;

void victim_main(void* arg) {
    Case* c = static_cast<Case*>(arg);
    for (;;) {
        osThreadFlagsWait(F_START, osFlagsWaitAny, osWaitForever);
        g_stage = 0; spin(g_k);
        g_stage = 1; g_pre = c->probe();
        g_stage = 2; c->victim_op();
        g_stage = 3; osThreadFlagsSet(c->runner, F_ACK);
        g_stage = 4;
    }
}

Outcome trial(Case& c, uint32_t k) {
    osThreadFlagsClear(F_ACK);
    c.reset();
    g_k = k; g_pre = false;
    osDelay(1);
    osThreadFlagsSet(c.victim, F_START);
    osDelay(1);
    g_seen = g_stage;
    c.aggressor_op();
    uint32_t r = osThreadFlagsWait(F_ACK, osFlagsWaitAny, T_ACK);
    Outcome o;
    if (r & osFlagsError) {
        bool stuck = c.stuck && c.stuck();
        if (c.rescue) c.rescue();
        uint32_t r2 = osThreadFlagsWait(F_ACK, osFlagsWaitAny, T_ACK);
        o = (stuck && !(r2 & osFlagsError)) ? BUG : ANOMALY;
    } else {
        o = g_pre ? LATE : EARLY;
    }
    if (o != ANOMALY && c.check && !c.check()) o = BUG;
    return o;
}

struct SweepResult { uint32_t kb, lo, hi, n[4], first_bug, last_bug; };

SweepResult& sweep(Case& c, uint32_t below, uint32_t above) {
    static SweepResult s; s = SweepResult{};
    uint32_t lo = 0, hi = BC_SEARCH_HI;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (trial(c, mid) == LATE) hi = mid; else lo = mid;
    }
    s.kb = hi; s.lo = (hi > below) ? hi - below : 0; s.hi = hi + above;
    s.first_bug = s.last_bug = UINT32_MAX;
    for (uint32_t k = s.lo; k <= s.hi; ++k) {
        Outcome o = trial(c, k);
        s.n[o]++;
        if (o == BUG) { if (s.first_bug == UINT32_MAX) s.first_bug = k; s.last_bug = k; }
        if (o == ANOMALY) { printf("   [%s] ANOMALY at k=%lu (stage %lu) -- sweep aborted\r\n", c.id, (unsigned long)k, (unsigned long)g_seen); break; }
    }
    printf("   [%s] kb=%lu k=%lu..%lu EARLY=%lu LATE=%lu BUG=%lu ANOMALY=%lu", c.id, (unsigned long)s.kb,
           (unsigned long)s.lo, (unsigned long)s.hi, (unsigned long)s.n[EARLY], (unsigned long)s.n[LATE],
           (unsigned long)s.n[BUG], (unsigned long)s.n[ANOMALY]);
    if (s.n[BUG]) printf(" (BUG k=%lu..%lu)", (unsigned long)s.first_bug, (unsigned long)s.last_bug);
    printf("\r\n");
    return s;
}

void sweep_verdict(Case& c, const char* what) {
    static SweepResult a, b;
    // The EARLY/LATE boundary search can land up to ~600 iterations below the
    // true boundary (per-trial state shifts timing), so sweep well above kb.
    a = sweep(c, BC_SWEEP_BELOW, BC_SWEEP_ABOVE); b = sweep(c, BC_SWEEP_BELOW, BC_SWEEP_ABOVE);
    bool ok = a.n[BUG] == 0 && b.n[BUG] == 0 && a.n[ANOMALY] == 0 && b.n[ANOMALY] == 0
              && a.n[EARLY] > 0 && a.n[LATE] > 0;            // both regimes covered
    result(c.id, ok ? 1 : 0, what);
}

// ---------------------------------------------------------------------------
// T0 -- control: Block buffered channel read through ALT
// ---------------------------------------------------------------------------
BlockChan<4>* t0_ch; ThreadSlot t0_slot;
void t0_writer(void*) { Out out(t0_ch); for (uint32_t i = 1; i <= 10; ++i) { out << i; osDelay(1); } park(); }
void test_T0() {
    static BlockChan<4> ch; t0_ch = &ch;
    spawn(t0_writer, nullptr, osPriorityNormal, t0_slot, "T0w");
    In in(&ch); uint32_t v = 0, sum = 0, n = 0;
    csp::Alternative alt BC_ALTV(BC_G(in, v));
    for (int i = 0; i < 10; ++i) { alt.priSelect(); sum += v; n++; }
    result("T0", (n == 10 && sum == 55) ? 1 : 0, "control: 10 values through a Block BufferedChannel via ALT");
}

// ---------------------------------------------------------------------------
// T1a / T1b -- lost wakeup in ALT enable (input / output guard)
// ---------------------------------------------------------------------------
BlockChan<4>* t1a_ch; ThreadSlot t1a_slot; Case t1a;
void t1a_reset() { drain(*t1a_ch); }
bool t1a_probe() { return t1a_ch->pending(); }
void t1a_victim() {
    static In in(t1a_ch); static uint32_t v;
    static csp::Alternative alt BC_ALTV(BC_G(in, v));
    alt.priSelect();
    drain(*t1a_ch);
}
void t1a_aggr()   { uint32_t v = 1; t1a_ch->output(&v); }
bool t1a_stuck()  { return t1a_ch->pending(); }
void t1a_rescue() { uint32_t kick = 0xFFFFFFFFu; t1a_ch->output(&kick); }
void test_T1a() {
    static BlockChan<4> ch; t1a_ch = &ch;
    t1a = {"T1a", t1a_reset, t1a_probe, t1a_victim, t1a_aggr, t1a_stuck, t1a_rescue, nullptr, nullptr, osThreadGetId()};
    t1a.victim = spawn(victim_main, &t1a, osPriorityLow, t1a_slot, "T1a");
    sweep_verdict(t1a, "no lost wakeup when a writer preempts an ALT reader anywhere in select()");
}

BlockChan<1>* t1b_ch; ThreadSlot t1b_slot; Case t1b;
void t1b_reset() { drain(*t1b_ch); uint32_t one = 1; t1b_ch->output(&one); }
bool t1b_probe() { return t1b_ch->space_available(); }
void t1b_victim() {
    static Out out(t1b_ch); static uint32_t w = 77;
    static csp::Alternative alt BC_ALTV(BC_G(out, w));
    alt.priSelect();
}
void t1b_aggr()   { uint32_t x; t1b_ch->input(&x); }
bool t1b_stuck()  { return t1b_ch->space_available(); }
void t1b_rescue() { uint32_t f = 5, x; t1b_ch->output(&f); t1b_ch->input(&x); }
void test_T1b() {
    static BlockChan<1> ch; t1b_ch = &ch;
    t1b = {"T1b", t1b_reset, t1b_probe, t1b_victim, t1b_aggr, t1b_stuck, t1b_rescue, nullptr, nullptr, osThreadGetId()};
    t1b.victim = spawn(victim_main, &t1b, osPriorityLow, t1b_slot, "T1b");
    sweep_verdict(t1b, "no lost wakeup when a reader preempts an ALT writer anywhere in select()");
}

// ---------------------------------------------------------------------------
// T2 -- no CMSIS-RTOS2 call while a CSP critical section is active
// Workload covers every notification path: ALT reader woken by output(),
// ALT writer woken by input(), KeepNewest overwrite, putFromISR() into a
// buffered channel with an ALT reader, and rendezvous putFromISR().
// ---------------------------------------------------------------------------
BlockChan<1>* t2_b; NewestChan<1>* t2_n; csp::Channel<uint32_t>* t2_r;
ThreadSlot t2_s1, t2_s2, t2_s3, t2_s4;
volatile uint32_t t2_got = 0;
void t2_alt_reader(void*) {            // ALT reader on the Block channel, 2 rounds
    In in(t2_b); uint32_t v; csp::Alternative alt BC_ALTV(BC_G(in, v));
    for (int i = 0; i < 2; ++i) { alt.priSelect(); t2_got = t2_got + v; }
    park();
}
void t2_alt_writer(void*) {            // ALT writer on the (full) Block channel
    Out out(t2_b); uint32_t w = 7; csp::Alternative alt BC_ALTV(BC_G(out, w));
    alt.priSelect(); park();
}
void t2_newest_reader(void*) {         // ALT reader on KeepNewest, 2 rounds
    In in(t2_n); uint32_t v; csp::Alternative alt BC_ALTV(BC_G(in, v));
    for (int i = 0; i < 2; ++i) { alt.priSelect(); t2_got = t2_got + v; }
    park();
}
void t2_isr_buffered() { (void)isr_put(t2_b, 1000u); }
void t2_isr_newest()   { (void)isr_put(t2_n, 2000u); }
#if BC_OWRV
BC_SignalChannel* t2_sig; ThreadSlot t2_s5, t2_s6, t2_s7;
void t2_rv_alt_reader(void*) { In in = t2_r->reader(); uint32_t v = 0; csp::Alternative alt BC_ALTV(BC_G(in, v)); alt.priSelect(); t2_got = t2_got + v; park(); }
void t2_rv_alt_writer(void*) { Out out = t2_r->writer(); uint32_t w = 4000; csp::Alternative alt BC_ALTV(BC_G(out, w)); alt.priSelect(); park(); }
void t2_sig_alt(void*) { csp::Chanin<csp::Signal> in = t2_sig->reader(); csp::Signal s; csp::Alternative alt BC_ALTV(BC_G(in, s)); alt.priSelect(); t2_got = t2_got + 1; park(); }
#endif
#if !BC_ISR_WRITER
void t2_rv_reader(void*) { uint32_t v; csp::Chanin<uint32_t> in = t2_r->reader(); in >> v; t2_got = t2_got + v; park(); }
void t2_isr_rv()       { uint32_t v = 3000; Out out = t2_r->writer(); out.putFromISR(v); }
#endif
void test_T2() {
    static BlockChan<1> b; static NewestChan<1> n; static csp::Channel<uint32_t> r;
    t2_b = &b; t2_n = &n; t2_r = &r;
    g_rtos_in_crit = 0; g_rtos_in_crit_fn = nullptr;
    // (1) ALT reader woken by task output(), then by putFromISR()
    spawn(t2_alt_reader, nullptr, osPriorityAboveNormal, t2_s1, "T2ar");
    osDelay(2); { uint32_t v = 1; b.output(&v); } osDelay(2);
    g_isr_op = t2_isr_buffered; isr_fire(); osDelay(2);
    // (2) ALT writer woken by task input()
    { uint32_t v = 3; b.output(&v); }                        // full again
    spawn(t2_alt_writer, nullptr, osPriorityAboveNormal, t2_s2, "T2aw"); osDelay(2);
    { uint32_t x; b.input(&x); } osDelay(2); drain(b);
    // (3) KeepNewest: ALT reader woken by task output() (overwrite path first), then by ISR
    spawn(t2_newest_reader, nullptr, osPriorityAboveNormal, t2_s3, "T2nr");
    osDelay(2); { uint32_t v = 10, w = 20; n.output(&v); n.output(&w); } osDelay(2);
    g_isr_op = t2_isr_newest; isr_fire(); osDelay(2);
#if !BC_ISR_WRITER
    // (4) rendezvous putFromISR() to a blocked reader (1.x only: 2.0 has no ISR path into rendezvous)
    spawn(t2_rv_reader, nullptr, osPriorityAboveNormal, t2_s4, "T2rv"); osDelay(2);
    g_isr_op = t2_isr_rv; isr_fire(); osDelay(2);
#else
    (void)t2_s4;
    // (4) 2.0 rendezvous and signal channels (task-only, OWRV): plain writer -> ALT reader,
    //     ALT writer -> plain reader, ALT writer vs ALT reader, signal to an ALT reader
    static BC_SignalChannel sig; t2_sig = &sig;
    spawn(t2_rv_alt_reader, nullptr, osPriorityAboveNormal, t2_s5, "T2ra"); osDelay(2);
    { Out o = r.writer(); o << 3000u; } osDelay(2);
    spawn(t2_rv_alt_writer, nullptr, osPriorityAboveNormal, t2_s6, "T2wa"); osDelay(2);
    { In i = r.reader(); uint32_t x = 0; i >> x; t2_got = t2_got + x; } osDelay(2);
    spawn(t2_rv_alt_writer, nullptr, osPriorityAboveNormal, t2_s7, "T2wb"); osDelay(2);
    { In i = r.reader(); uint32_t x = 0; csp::Alternative alt BC_ALTV(BC_G(i, x)); alt.priSelect(); t2_got = t2_got + x; } osDelay(2);
    static ThreadSlot t2_s8; spawn(t2_sig_alt, nullptr, osPriorityAboveNormal, t2_s8, "T2sg"); osDelay(2);
    { csp::Chanout<csp::Signal> o = sig.writer(); o << csp::Signal{}; } osDelay(2);
#endif
    g_isr_op = nullptr;
    printf("   [T2] RTOS calls with BASEPRI raised: %lu (last: %s); workload sum=%lu\r\n",
           (unsigned long)g_rtos_in_crit, g_rtos_in_crit_fn ? g_rtos_in_crit_fn : "-", (unsigned long)t2_got);
    result("T2", g_rtos_in_crit == 0 ? 1 : 0, "no CMSIS-RTOS2 call inside a CSP critical section (BASEPRI raised)");
}

// ---------------------------------------------------------------------------
// T3 / T3i -- KeepNewest atomicity: task vs task, task vs ISR
// capacity 2, full {1,2}; victim writes 100, aggressor 200; both must survive
// ---------------------------------------------------------------------------
NewestChan<2>* t3_ch; ThreadSlot t3_slot, t3i_slot; Case t3, t3i;
volatile bool t3_aggr_done = false;
void t3_reset() { drain(*t3_ch); uint32_t a = 1, b = 2; t3_ch->output(&a); t3_ch->output(&b); t3_aggr_done = false; }
bool t3_probe() { return t3_aggr_done; }
void t3_victim() { static Out out(t3_ch); out << 100u; }
void t3_aggr()   { uint32_t v = 200; t3_ch->output(&v); t3_aggr_done = true; }
void t3_isr()    { (void)isr_put(t3_ch, 200u); t3_aggr_done = true; }
void t3i_aggr()  { g_isr_op = t3_isr; isr_fire(); }
bool t3_check() {
    bool has100 = false, has200 = false; uint32_t x;
    while (t3_ch->pending()) { t3_ch->input(&x); has100 |= (x == 100); has200 |= (x == 200); }
    return has100 && has200;
}
void test_T3() {
    static NewestChan<2> ch; t3_ch = &ch;
    t3  = {"T3",  t3_reset, t3_probe, t3_victim, t3_aggr,  nullptr, nullptr, t3_check, nullptr, osThreadGetId()};
    t3.victim = spawn(victim_main, &t3, osPriorityLow, t3_slot, "T3");
    sweep_verdict(t3, "KeepNewest keeps the newest value of every writer (task vs task)");
}
void test_T3i() {
    t3i = {"T3i", t3_reset, t3_probe, t3_victim, t3i_aggr, nullptr, nullptr, t3_check, nullptr, osThreadGetId()};
    t3i.victim = spawn(victim_main, &t3i, osPriorityLow, t3i_slot, "T3i");
    sweep_verdict(t3i, "KeepNewest keeps the newest value of every writer (task vs ISR putFromISR)");
    g_isr_op = nullptr;
}

// ---------------------------------------------------------------------------
// T13 / T13b -- no livelock: a HIGH-priority ALT process meets a LOW-priority
// blocking partner that was preempted in the middle of its operation (e.g.
// between the push and the semaphore release). The ALT must block, not spin.
// Aggressor = separate thread (above the victim, below the runner) woken at
// E1; the runner detects a spin (no progress within T_ACK while the victim
// is stuck) and rescues by temporarily dropping the aggressor's priority.
// ---------------------------------------------------------------------------
constexpr uint32_t F_GO   = 0x2u;     // runner -> aggressor thread (application bit)
constexpr uint32_t F_ACK2 = 0x4u;     // aggressor thread -> runner
BlockChan<1>* t13_ch; ThreadSlot t13_vs, t13_as, t13b_vs, t13b_as; Case t13, t13b;
osThreadId_t t13_aggr_tid; volatile bool t13_go = false, t13_livelock = false;
uint32_t t13_livelocks = 0;
void t13_reader_aggr(void*) {                     // high-priority ALT reader
    In in(t13_ch); uint32_t v = 0; csp::Alternative alt BC_ALTV(BC_G(in, v));
    for (;;) { osThreadFlagsWait(F_GO, osFlagsWaitAny, osWaitForever); alt.priSelect(); osThreadFlagsSet(t13.runner, F_ACK2); }
}
void t13_writer_aggr(void*) {                     // high-priority ALT writer
    Out out(t13_ch); uint32_t w = 9; csp::Alternative alt BC_ALTV(BC_G(out, w));
    for (;;) { osThreadFlagsWait(F_GO, osFlagsWaitAny, osWaitForever); alt.priSelect(); osThreadFlagsSet(t13b.runner, F_ACK2); }
}
void t13_kick() {
    t13_go = true;
    osThreadFlagsClear(F_ACK2);
    osThreadFlagsSet(t13_aggr_tid, F_GO);
    if (osThreadFlagsWait(F_ACK2, osFlagsWaitAny, T_ACK) & osFlagsError) {   // no progress: spinning?
        t13_livelock = true; t13_livelocks++;
        osThreadSetPriority(t13_aggr_tid, (osPriority_t)(osPriorityLow - 1));  // let the victim finish
        (void)osThreadFlagsWait(F_ACK2, osFlagsWaitAny, T_ACK);
        osThreadSetPriority(t13_aggr_tid, osPriorityAboveNormal);
    }
}
bool t13_probe() { return t13_go; }
bool t13_check() { return !t13_livelock; }
void t13_reset()  { drain(*t13_ch); t13_go = false; t13_livelock = false; }
void t13b_reset() { drain(*t13_ch); uint32_t one = 1; t13_ch->output(&one); t13_go = false; t13_livelock = false; }
void t13_victim()  { static Out out(t13_ch); out << 5u; }             // blocking writer
void t13b_victim() { uint32_t x; t13_ch->input(&x); }                 // blocking reader
void test_T13() {
    static BlockChan<1> ch; t13_ch = &ch; t13_livelocks = 0;
    t13 = {"T13", t13_reset, t13_probe, t13_victim, t13_kick, nullptr, nullptr, t13_check, nullptr, osThreadGetId()};
    t13_aggr_tid = spawn(t13_reader_aggr, nullptr, osPriorityAboveNormal, t13_as, "T13A");
    t13.victim   = spawn(victim_main, &t13, osPriorityLow, t13_vs, "T13V");
    sweep_verdict(t13, "no livelock: high-priority ALT reader vs preempted low-priority blocking writer");
    printf("   [T13] spins detected: %lu\r\n", (unsigned long)t13_livelocks);
}
void test_T13b() {
    static BlockChan<1> ch; t13_ch = &ch; t13_livelocks = 0;
    t13b = {"T13b", t13b_reset, t13_probe, t13b_victim, t13_kick, nullptr, nullptr, t13_check, nullptr, osThreadGetId()};
    t13_aggr_tid = spawn(t13_writer_aggr, nullptr, osPriorityAboveNormal, t13b_as, "T13bA");
    t13b.victim  = spawn(victim_main, &t13b, osPriorityLow, t13b_vs, "T13bV");
    sweep_verdict(t13b, "no livelock: high-priority ALT writer vs preempted low-priority blocking reader");
    printf("   [T13b] spins detected: %lu\r\n", (unsigned long)t13_livelocks);
}

// ---------------------------------------------------------------------------
// T4a -- two ALT writers on one channel: either both are served, or the
//        second is rejected by the v2 assert; never a silent hang
// T4b -- an unrelated ALT must not cancel another writer's registration
// T4c -- guard state per writer: a second writer's getGuard() must not
//        re-target a blocked writer's ALT
// ---------------------------------------------------------------------------
BlockChan<1>* t4_ch; ThreadSlot t4_sa, t4_sb; volatile int t4_doneA = 0, t4_doneB = 0;
void t4_writerA(void*) { Out out(t4_ch); uint32_t a = 10; csp::Alternative alt BC_ALTV(BC_G(out, a)); alt.priSelect(); t4_doneA = 1; park(); }
void t4_writerB(void*) { osDelay(2); Out out(t4_ch); uint32_t b = 20; csp::Alternative alt BC_ALTV(BC_G(out, b)); alt.priSelect(); t4_doneB = 1; park(); }
void test_T4a() {
    static BlockChan<1> ch; t4_ch = &ch; uint32_t x = 1; ch.output(&x);
    uint32_t fatal0 = g_fatal_count;
    spawn(t4_writerA, nullptr, osPriorityNormal, t4_sa, "T4aA");
    spawn(t4_writerB, nullptr, osPriorityNormal, t4_sb, "T4aB");
    osDelay(6);
    bool rejected = g_fatal_count != fatal0;
    uint32_t v1 = 0, v2 = 0; ch.input(&v1); osDelay(3);
    if (ch.pending()) ch.input(&v2);
    osDelay(3);
    if (!rejected && ch.pending()) { uint32_t v3; ch.input(&v3); osDelay(3); }
    printf("   [T4a] second ALT writer rejected by assert=%d (%s); A done=%d, B done=%d\r\n", (int)rejected,
           rejected && g_fatal_msg ? g_fatal_msg : "-", t4_doneA, t4_doneB);
    bool ok = rejected ? (t4_doneA == 1) : (t4_doneA == 1 && t4_doneB == 1);
    result("T4a", ok ? 1 : 0, "two ALT writers: both served or the second rejected by assert, never a silent hang");
}

BlockChan<1>* t4b_ch; ThreadSlot t4b_sa; volatile int t4b_doneA = 0;
void t4b_writerA(void*) { Out out(t4b_ch); uint32_t a = 10; csp::Alternative alt BC_ALTV(BC_G(out, a)); alt.priSelect(); t4b_doneA = 1; park(); }
void test_T4b() {
    static BlockChan<1> ch; t4b_ch = &ch; uint32_t x = 1; ch.output(&x);
    spawn(t4b_writerA, nullptr, osPriorityNormal, t4b_sa, "T4bA");
    osDelay(3);
    {   // unrelated ALT: timeout guard ready first -> output guard never enabled
        static Out outB(&ch); uint32_t b = 20;
        static csp::RelTimeoutGuard instant(csp::Time(0));
        csp::Alternative alt BC_ALTV(BC_T(instant), BC_G(outB, b));
        (void)alt.priSelect();
    }
    uint32_t v; ch.input(&v); osDelay(5);
    result("T4b", t4b_doneA == 1 ? 1 : 0, "an ALT that never enabled a guard does not cancel another writer's registration");
}

BlockChan<1>* t4c_ch; ThreadSlot t4c_sa; volatile int t4c_doneA = 0;
void t4c_writerA(void*) { Out out(t4c_ch); uint32_t a = 10; csp::Alternative alt BC_ALTV(BC_G(out, a)); alt.priSelect(); t4c_doneA = 1; park(); }
void test_T4c() {
    static BlockChan<1> ch; t4c_ch = &ch; uint32_t x = 1; ch.output(&x);
    spawn(t4c_writerA, nullptr, osPriorityNormal, t4c_sa, "T4cA");
    osDelay(3);
    static Out outB(&ch); static uint32_t b = 20;
    (void)BC_OUT_GUARD(outB, b);                        // another writer builds its guard
    uint32_t v1, v2 = 0; ch.input(&v1); osDelay(3);
    if (ch.pending()) ch.input(&v2);
    printf("   [T4c] writer A (sends 10) done=%d; reader received %lu\r\n", t4c_doneA, (unsigned long)v2);
    result("T4c", (t4c_doneA == 1 && v2 == 10) ? 1 : 0, "guard state is per writer: A's ALT sends A's value");
}

// ---------------------------------------------------------------------------
// T5 -- a buffered channel is either valid after construction or construction
//       fails loudly; never a silently dead channel. 8200 x 4 B > 32 KB heap
//       (v1 queue) -- in v2 the storage is static.
// ---------------------------------------------------------------------------
void test_T5() {
    uint32_t used0 = heap_used(), fatal0 = g_fatal_count;
    static BlockChan<8200> big;
    bool valid = !big.pending() && big.space_available();
    printf("   [T5] after construction: pending=%d space_available=%d fatal=%lu heap delta=%ld\r\n",
           (int)big.pending(), (int)big.space_available(), (unsigned long)(g_fatal_count - fatal0),
           (long)heap_used() - (long)used0);
    result("T5", (valid || g_fatal_count != fatal0) ? 1 : 0, "large channel is valid (or construction fails loudly)");
}

// ---------------------------------------------------------------------------
// T6 -- timeout guards and Alternatives create no RTOS object and use no
//       RTOS heap (2.0.1: no timer; 2.0.0: a static timer, but a timer)
// ---------------------------------------------------------------------------
void test_T6() {
    uint32_t u0 = heap_used(), u_in = 0, u_alt = 0, allocs = 0, tn0 = g_timer_news;
    { csp::RelTimeoutGuard g(csp::Time(5)); u_in = heap_used(); }
    { static BlockChan<1> ch; static In in(&ch); uint32_t v; uint32_t b = heap_used();
      csp::Alternative alt BC_ALTV(BC_G(in, v)); u_alt = heap_used() - b; }
    for (int i = 0; i < 200; ++i) { uint32_t b = heap_used(); csp::RelTimeoutGuard g(csp::Time(5)); if (heap_used() > b) allocs++; }
    uint32_t timers = g_timer_news - tn0;
    printf("   [T6] RelTimeoutGuard: %lu B heap while alive; Alternative: %lu B; 200 loop constructions -> %lu allocations, %lu RTOS timers\r\n",
           (unsigned long)(u_in - u0), (unsigned long)u_alt, (unsigned long)allocs, (unsigned long)timers);
    result("T6", (u_in == u0 && u_alt == 0 && allocs == 0 && timers == 0) ? 1 : 0,
           "RelTimeoutGuard and Alternative create no RTOS timer and use no RTOS heap");
}

// ---------------------------------------------------------------------------
// T17 -- rendezvous and signal channels use no RTOS heap (2.0, static
//        allocation): 50 constructions/destructions of each
// ---------------------------------------------------------------------------
void test_T17() {
#if BC_OWRV
    uint32_t allocs = 0, fatal0 = g_fatal_count, u0 = heap_used();
    for (int i = 0; i < 50; ++i) {
        uint32_t b = heap_used();
        { csp::Channel<uint32_t> c; BC_SignalChannel s; csp::Barrier bar(3); if (heap_used() > b) allocs++; }
    }
    printf("   [T17] 50 x (Channel + SignalChannel + Barrier): %lu allocations, heap delta %ld B, fatal=%lu\r\n",
           (unsigned long)allocs, (long)heap_used() - (long)u0, (unsigned long)(g_fatal_count - fatal0));
    result("T17", (allocs == 0 && g_fatal_count == fatal0) ? 1 : 0, "rendezvous and signal channels and Barrier use no RTOS heap");
#else
    result("T17", -1, "rendezvous and signal channels use no RTOS heap (2.0 only)");
#endif
}

// ---------------------------------------------------------------------------
// T18 -- Barrier(3), reused for 20 phases by threads of different priority:
//        nobody leaves phase p before all three arrived in phase p
// ---------------------------------------------------------------------------
constexpr uint32_t T18_N = 3, T18_PHASES = 20;
csp::Barrier* t18_bar; ThreadSlot t18_s[T18_N];
volatile uint8_t t18_arrived[T18_N][T18_PHASES]; volatile uint32_t t18_early[T18_N], t18_done[T18_N];
void t18_worker(void* arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    for (uint32_t ph = 0; ph < T18_PHASES; ++ph) {
        t18_arrived[id][ph] = 1;
        if (id == 0 && (ph % 3) == 0) osDelay(1);            // a slow process now and then
        t18_bar->sync();
        for (uint32_t j = 0; j < T18_N; ++j)
            if (!t18_arrived[j][ph]) t18_early[id] = t18_early[id] + 1;   // left phase ph too early
    }
    t18_done[id] = 1; park();
}
void test_T18() {
    static csp::Barrier bar(T18_N); t18_bar = &bar;
    const osPriority_t prio[T18_N] = { osPriorityLow, osPriorityNormal, osPriorityAboveNormal };
    for (uint32_t i = 0; i < T18_N; ++i) spawn(t18_worker, (void*)(uintptr_t)i, prio[i], t18_s[i], "T18");
    osDelay(150);
    uint32_t early = 0, done = 0;
    for (uint32_t i = 0; i < T18_N; ++i) { early += t18_early[i]; done += t18_done[i]; }
    printf("   [T18] %lu threads x %lu phases: finished=%lu, early departures=%lu\r\n",
           (unsigned long)T18_N, (unsigned long)T18_PHASES, (unsigned long)done, (unsigned long)early);
    result("T18", (done == T18_N && early == 0) ? 1 : 0, "reusable Barrier: nobody leaves a phase before all arrived");
}

// ---------------------------------------------------------------------------
// T19 -- heap-free build types only (RTOS dynamic allocation disabled): no
//        dynamic RTOS allocation was even attempted during the whole suite.
//        FreeRTOS-NoHeap: the harness's pvPortMalloc()/vPortFree() traps
//        were never called. RTX5-NoHeap: there is no dynamic memory pool
//        (any dynamic creation would have returned NULL -> fatal).
//        Run last. Not run (no RESULT line) in builds with a heap.
// ---------------------------------------------------------------------------
extern "C" __attribute__((weak)) volatile unsigned int noheap_alloc_calls;
void test_T19() {
#if defined(BC_FREERTOS) && (configSUPPORT_DYNAMIC_ALLOCATION == 0)
    if (&noheap_alloc_calls == nullptr) {
        // The linker removed the traps: nothing in the image references
        // pvPortMalloc()/vPortFree() at all (strongest outcome).
        printf("   [T19] FreeRTOS heap-free build: pvPortMalloc()/vPortFree() traps not linked (no reference in the image)\r\n");
        result("T19", 1, "heap-free build: no dynamic RTOS allocation possible (allocator not referenced)");
    } else {
        unsigned int calls = noheap_alloc_calls;
        printf("   [T19] FreeRTOS heap-free build: pvPortMalloc()/vPortFree() trap calls during the suite: %u\r\n", calls);
        result("T19", calls == 0 ? 1 : 0, "heap-free build: no dynamic RTOS allocation attempted");
    }
#elif defined(BC_RTX5)
    if (osRtxInfo.mem.common == nullptr) {
        printf("   [T19] RTX5 heap-free build: no dynamic memory pool (osRtxInfo.mem.common == NULL); fatal errors=%lu\r\n",
               (unsigned long)g_fatal_count);
        result("T19", 1, "heap-free build: no dynamic RTOS memory exists; every object was created statically");
    }
#endif
}

// ---------------------------------------------------------------------------
// T7a -- a read via ALT wakes a writer blocked in ALT (control: plain input)
// ---------------------------------------------------------------------------
BlockChan<1>* t7_ch; ThreadSlot t7_s1, t7_s2; volatile int t7_done = 0;
void t7_writer(void*) { Out out(t7_ch); uint32_t w = 10; csp::Alternative alt BC_ALTV(BC_G(out, w)); alt.priSelect(); t7_done = 1; park(); }
bool t7_run(bool reader_uses_alt, ThreadSlot& slot) {
    static BlockChan<1> chA, chB;
    BlockChan<1>& ch = reader_uses_alt ? chA : chB; t7_ch = &ch; t7_done = 0;
    uint32_t x = 1; ch.output(&x);
    spawn(t7_writer, nullptr, osPriorityNormal, slot, reader_uses_alt ? "T7alt" : "T7ctl");
    osDelay(3);
    uint32_t r = 0;
    if (reader_uses_alt) { static In in(&chA); csp::Alternative alt BC_ALTV(BC_G(in, r)); alt.priSelect(); }
    else                 { ch.input(&r); }
    osDelay(5);
    return t7_done == 1;
}
void test_T7a() {
    bool ctl = t7_run(false, t7_s1), alt = t7_run(true, t7_s2);
    printf("   [T7a] writer woken: reader uses input()=%d, reader uses ALT=%d\r\n", ctl, alt);
    result("T7a", (ctl && alt) ? 1 : 0, "a read via ALT wakes a writer blocked in ALT");
}

// ---------------------------------------------------------------------------
// T8 -- any number of blocking writers (plain output) on one channel
// ---------------------------------------------------------------------------
BlockChan<2>* t8_ch; ThreadSlot t8_s[3]; volatile uint32_t t8_done = 0;
void t8_writer(void* arg) {
    uint32_t base = (uint32_t)(uintptr_t)arg; Out out(t8_ch);
    for (uint32_t i = 1; i <= 20; ++i) out << (base + i);
    t8_done = t8_done + 1; park();
}
void test_T8() {
    static BlockChan<2> ch; t8_ch = &ch;
    for (uint32_t w = 0; w < 3; ++w) spawn(t8_writer, (void*)(uintptr_t)(w * 1000), osPriorityNormal, t8_s[w], "T8w");
    uint64_t sum = 0; uint32_t x;
    for (int i = 0; i < 60; ++i) { ch.input(&x); sum += x; }
    osDelay(3);
    const uint64_t expect = 3 * 210 + 20 * (0 + 1000 + 2000);
    printf("   [T8] 3 writers x 20 values: sum=%llu (expect %llu), writers done=%lu\r\n",
           (unsigned long long)sum, (unsigned long long)expect, (unsigned long)t8_done);
    result("T8", (sum == expect && t8_done == 3) ? 1 : 0, "three blocking writers on one Block channel, no loss");
}

// ---------------------------------------------------------------------------
// T9 -- stale ALT wakeup (v2): a flag for guard 0 without data must not make
//       select() return; real data afterwards must be delivered.
// ---------------------------------------------------------------------------
#if LIB_V2
BlockChan<1>* t9_ch; ThreadSlot t9_s; volatile uint32_t t9_ret = 0, t9_val = 0; osThreadId_t t9_tid;
void t9_reader(void*) {
    In in(t9_ch); uint32_t v = 0; csp::Alternative alt BC_ALTV(BC_G(in, v));
    alt.priSelect(); t9_val = v; t9_ret = t9_ret + 1; park();
}
#endif
void test_T9() {
#if LIB_V2
    static BlockChan<1> ch; t9_ch = &ch;
    t9_tid = spawn(t9_reader, nullptr, osPriorityNormal, t9_s, "T9");
    osDelay(3);
    osThreadFlagsSet(t9_tid, csp::internal::altFlag(0));   // stale wakeup, no data
    osDelay(5);
    uint32_t early = t9_ret;
    uint32_t v = 42; ch.output(&v); osDelay(3);
    printf("   [T9] returns after stale flag=%lu; after real data: returns=%lu value=%lu\r\n",
           (unsigned long)early, (unsigned long)t9_ret, (unsigned long)t9_val);
    result("T9", (early == 0 && t9_ret == 1 && t9_val == 42) ? 1 : 0, "stale ALT wakeup is re-verified, not selected");
#else
    result("T9", -1, "stale ALT wakeup re-verification (needs v2 altFlag())");
#endif
}

// ---------------------------------------------------------------------------
// T10 -- a second ALTing reader on one channel is rejected (assert)
// ---------------------------------------------------------------------------
BlockChan<1>* t10_ch; ThreadSlot t10_s1, t10_s2; volatile int t10_done1 = 0, t10_done2 = 0;
void t10_r1(void*) { In in(t10_ch); uint32_t v; csp::Alternative alt BC_ALTV(BC_G(in, v)); alt.priSelect(); t10_done1 = 1; park(); }
void t10_r2(void*) { osDelay(2); In in(t10_ch); uint32_t v; csp::Alternative alt BC_ALTV(BC_G(in, v)); alt.priSelect(); t10_done2 = 1; park(); }
void test_T10() {
    static BlockChan<1> ch; t10_ch = &ch;
    uint32_t fatal0 = g_fatal_count;
    spawn(t10_r1, nullptr, osPriorityNormal, t10_s1, "T10a");
    spawn(t10_r2, nullptr, osPriorityNormal, t10_s2, "T10b");
    osDelay(5);
    bool rejected = g_fatal_count != fatal0;
    uint32_t v = 5; ch.output(&v); osDelay(3);
    printf("   [T10] second ALT reader rejected=%d (%s); reader1 done=%d reader2 done=%d\r\n", (int)rejected,
           rejected && g_fatal_msg ? g_fatal_msg : "-", t10_done1, t10_done2);
    result("T10", (rejected && t10_done1 == 1) ? 1 : 0, "a second ALTing reader is rejected by assert; the first is served");
}

// ---------------------------------------------------------------------------
// T11 -- rendezvous ALT-vs-ALT (both ends select, each with a timeout guard)
// ---------------------------------------------------------------------------
csp::Channel<uint32_t>* t11_ch; ThreadSlot t11_s; volatile int t11_wsel = -1, t11_wdone = 0;
void t11_writer(void*) {
    Out out = t11_ch->writer(); uint32_t w = 77;
    csp::RelTimeoutGuard to(csp::Time(50));
    csp::Alternative alt BC_ALTV(BC_G(out, w), BC_T(to));
    t11_wsel = alt.priSelect(); t11_wdone = 1; park();
}
void test_T11() {
    static csp::Channel<uint32_t> ch; t11_ch = &ch;
    spawn(t11_writer, nullptr, osPriorityNormal, t11_s, "T11w");
    osDelay(3);                                          // writer is waiting in its ALT
    In in = ch.reader(); uint32_t r = 0;
    csp::RelTimeoutGuard to(csp::Time(50));
    csp::Alternative alt BC_ALTV(BC_G(in, r), BC_T(to));
    int sel = alt.priSelect(); osDelay(3);
    printf("   [T11] reader selected %d (value %lu); writer selected %d, done=%d (0 = channel, 1 = timeout)\r\n",
           sel, (unsigned long)r, t11_wsel, t11_wdone);
    result("T11", (sel == 0 && r == 77 && t11_wsel == 0 && t11_wdone == 1) ? 1 : 0,
           "rendezvous ALT-vs-ALT completes on both ends with the value");
}

// ---------------------------------------------------------------------------
// T12 -- rendezvous fairSelect over two channels, blocking senders (demo
//        pattern), pipe syntax; per-channel order and count
// ---------------------------------------------------------------------------
ThreadSlot t12_s1, t12_s2;
void t12_sender(void* arg) {
    Out out = static_cast<csp::Channel<uint32_t>*>(arg)->writer();
    for (uint32_t i = 1; i <= 500; ++i) out << i;
    park();
}
void test_T12() {
    static csp::Channel<uint32_t> a, b;
    spawn(t12_sender, &a, osPriorityNormal, t12_s1, "T12a");
    spawn(t12_sender, &b, osPriorityNormal, t12_s2, "T12b");
    In ina = a.reader(), inb = b.reader(); uint32_t va = 0, vb = 0;
    csp::Alternative alt(ina | va, inb | vb);
    uint32_t na = 0, nb = 0; bool order_ok = true;
    for (int i = 0; i < 1000; ++i) {
        int sel = alt.fairSelect();
        if (sel == 0) { order_ok &= (va == na + 1); na++; }
        else if (sel == 1) { order_ok &= (vb == nb + 1); nb++; }
        else order_ok = false;
    }
    printf("   [T12] received a=%lu b=%lu, per-channel order ok=%d\r\n", (unsigned long)na, (unsigned long)nb, (int)order_ok);
    result("T12", (na == 500 && nb == 500 && order_ok) ? 1 : 0,
           "rendezvous fairSelect over two channels: all messages, in order");
}

// ---------------------------------------------------------------------------
// T15 family -- rendezvous / signal "trust the wakeup" path (CSP-M model,
// assertion 23: a flag that no partner produced for the current select()
// round makes a rendezvous guard complete without a data transfer).
// FAIL = the defect is present. Analysis only; the library is unchanged.
// ---------------------------------------------------------------------------
constexpr uint32_t T15_SENT = 0xDEADBEEFu;   // "nothing received" marker

#if !BC_ISR_WRITER
// T15i -- rendezvous putFromISR() to a waiting ALT reader: the ISR reports
// success, the reader's select() returns the channel guard. Correct: the
// reader has the ISR's value (or putFromISR() returns false and the reader
// times out).
csp::Channel<uint32_t>* t15i_ch; ThreadSlot t15i_s;
volatile int t15i_sel = -2; volatile uint32_t t15i_msg = 0; volatile bool t15i_isr_ok = false;
void t15i_reader(void*) {
    In in = t15i_ch->reader(); uint32_t msg = T15_SENT;
    csp::RelTimeoutGuard to(csp::Time(50));
    csp::Alternative alt BC_ALTV(BC_G(in, msg), BC_T(to));
    int s = alt.priSelect(); t15i_msg = msg; t15i_sel = s; park();
}
void t15i_isr() { Out out = t15i_ch->writer(); t15i_isr_ok = out.putFromISR(1234u); }
void test_T15i() {
    static csp::Channel<uint32_t> ch; t15i_ch = &ch;
    spawn(t15i_reader, nullptr, osPriorityNormal, t15i_s, "T15i");
    osDelay(3);                                          // reader waits in its ALT
    g_isr_op = t15i_isr; isr_fire(); osDelay(3); g_isr_op = nullptr;
    osDelay(60);                                         // past the reader's timeout
    bool phantom = t15i_sel == 0 && t15i_msg == T15_SENT;
    bool ok = (t15i_isr_ok && t15i_sel == 0 && t15i_msg == 1234u) || (!t15i_isr_ok && t15i_sel == 1);
    printf("   [T15i] putFromISR returned %d; reader selected %d (0 = channel, 1 = timeout), value %s%lx%s\r\n",
           (int)t15i_isr_ok, t15i_sel, t15i_msg == T15_SENT ? "UNCHANGED (0x" : "0x", (unsigned long)t15i_msg,
           t15i_msg == T15_SENT ? ")" : "");
    if (phantom) printf("   [T15i] rendezvous reported without data transfer; the ISR's value is lost\r\n");
    result("T15i", ok ? 1 : 0, "rendezvous putFromISR() to an ALT reader delivers the value (or reports failure)");
}

#else
void test_T15i() {
    replaced("T15i", "neg_rendezvous_isr.cpp",
             "rendezvous putFromISR() no longer exists (ISR writes go to buffered channels via isrWriter())");
}
#endif

// T15s -- SignalChannel ALT receiver {X (buffered, index 0), signal (index 1)}.
// X becomes ready and a sender signals before the receiver runs; the receiver
// takes X (lower index). The signal must still be received by the next
// select(), and the sender must complete.
BlockChan<1>* t15s_x; BC_SignalChannel* t15s_sig; ThreadSlot t15s_rs, t15s_ss;
osThreadId_t t15s_sender_tid; volatile int t15s_sel1 = -2, t15s_sel2 = -2, t15s_sent = 0;
#if BC_OWRV
void t15s_receiver(void*) {                  // 2.0: a signal is a data-less rendezvous
    In xin(t15s_x); uint32_t xv = 0;
    csp::Chanin<csp::Signal> sin = t15s_sig->reader(); csp::Signal sv;
    csp::Alternative alt1 BC_ALTV(BC_G(xin, xv), BC_G(sin, sv));
    t15s_sel1 = alt1.priSelect();
    csp::RelTimeoutGuard to(csp::Time(50));
    csp::Alternative alt2 BC_ALTV(BC_G(xin, xv), BC_G(sin, sv), BC_T(to));
    t15s_sel2 = alt2.priSelect();
    park();
}
void t15s_sender(void*) {
    osThreadFlagsWait(F_GO, osFlagsWaitAny, osWaitForever);
    csp::Chanout<csp::Signal> out = t15s_sig->writer(); out << csp::Signal{};
    t15s_sent = 1; park();
}
#else
void t15s_receiver(void*) {
    In xin(t15s_x); uint32_t xv = 0;
    auto* sig = t15s_sig->getInternal();
    csp::Alternative alt1 BC_ALTV(BC_G(xin, xv), sig->getInputGuard());
    t15s_sel1 = alt1.priSelect();
    csp::RelTimeoutGuard to(csp::Time(50));
    csp::Alternative alt2 BC_ALTV(BC_G(xin, xv), sig->getInputGuard(), BC_T(to));
    t15s_sel2 = alt2.priSelect();
    park();
}
void t15s_sender(void*) {
    osThreadFlagsWait(F_GO, osFlagsWaitAny, osWaitForever);
    t15s_sig->getInternal()->output(nullptr);
    t15s_sent = 1; park();
}
#endif
void test_T15s() {
    static BlockChan<1> x; static BC_SignalChannel sig; t15s_x = &x; t15s_sig = &sig;
    spawn(t15s_receiver, nullptr, osPriorityNormal, t15s_rs, "T15sR");
    t15s_sender_tid = spawn(t15s_sender, nullptr, osPriorityAboveNormal, t15s_ss, "T15sS");
    osDelay(3);                                          // receiver waits on both guards
    uint32_t one = 1; x.output(&one);                    // X ready (receiver not running yet)
    osThreadFlagsSet(t15s_sender_tid, F_GO);             // sender signals once the runner sleeps
    osDelay(80);                                         // receiver: 2 selects, the second may time out
    printf("   [T15s] select 1 -> %d (0 = X); select 2 -> %d (1 = signal, 2 = timeout); sender completed=%d\r\n",
           t15s_sel1, t15s_sel2, t15s_sent);
    if (t15s_sel2 == 2 && !t15s_sent) printf("   [T15s] signal lost: the sender is blocked for good\r\n");
    result("T15s", (t15s_sel1 == 0 && t15s_sel2 == 1 && t15s_sent == 1) ? 1 : 0,
           "a signal that arrives while the ALT receiver takes another guard is not lost");
}

// T15 (sweep) -- stale rendezvous wakeup from an earlier round. Target: ALT
// reader TA {C (rendezvous, index 0), X (buffered, index 1)}, looping; it
// resets its C destination to T15_SENT before every select(). Victim: ALT
// writer on C (low priority), which completes the ALT-vs-ALT rendezvous in
// its activate(): copy, clear TA's registration, release the mutex, THEN
// wake TA. Aggressor (runner at E1): X.output(). Priorities: runner > TA >
// victim. If the victim is preempted between its copy and its wake, TA takes
// X, starts a new select(), and the victim's late flag then lands in that
// round. Checked per trial, from TA's log:
//   PHANTOM: select() returned C with the destination unchanged (T15_SENT)
//   SILENT : select() returned X although C's data had been written
// Correct: exactly one C with the victim's value, one X, neither of the above.
csp::Channel<uint32_t>* t15_ch; BlockChan<1>* t15_x; ThreadSlot t15_ts, t15_vs; Case t15;
struct T15Entry { int sel; uint32_t msg; };
volatile T15Entry t15_log[8]; volatile uint32_t t15_n = 0;
volatile uint32_t t15_w = 0; volatile bool t15_aggr_done = false;
uint32_t t15_phantoms = 0, t15_silents = 0, t15_badtrials = 0;
void t15_target(void*) {
    In in = t15_ch->reader(); In xin(t15_x);
    static uint32_t msg, xv;
    csp::Alternative alt BC_ALTV(BC_G(in, msg), BC_G(xin, xv));
    for (;;) {
        msg = T15_SENT;
        int s = alt.priSelect();
        uint32_t n = t15_n;
        if (n < 8) { t15_log[n].sel = s; t15_log[n].msg = msg; }
        t15_n = n + 1;
    }
}
void t15_reset()  { t15_n = 0; t15_aggr_done = false; t15_w = t15_w + 1; }
bool t15_probe()  { return t15_aggr_done; }
void t15_victim() {
    static Out out = t15_ch->writer(); static uint32_t w;
    static csp::Alternative alt BC_ALTV(BC_G(out, w));
    w = t15_w;
    alt.priSelect();
}
void t15_aggr()   { uint32_t one = 1; t15_x->output(&one); t15_aggr_done = true; }
bool t15_check() {
    osDelay(2);                                          // let TA finish (it runs above the victim)
    uint32_t n = t15_n, ph = 0, si = 0, c_ok = 0, x_n = 0;
    for (uint32_t i = 0; i < n && i < 8; ++i) {
        if (t15_log[i].sel == 0) { if (t15_log[i].msg == T15_SENT) ph++; else if (t15_log[i].msg == t15_w) c_ok++; }
        else if (t15_log[i].sel == 1) { x_n++; if (t15_log[i].msg != T15_SENT) si++; }
    }
    t15_phantoms += ph; t15_silents += si;
    bool ok = n == 2 && c_ok == 1 && x_n == 1 && ph == 0 && si == 0;
    if (!ok) t15_badtrials++;
    return ok;
}
void test_T15() {
    static csp::Channel<uint32_t> ch; static BlockChan<1> x; t15_ch = &ch; t15_x = &x;
    t15_phantoms = t15_silents = t15_badtrials = 0;
    t15 = {"T15", t15_reset, t15_probe, t15_victim, t15_aggr, nullptr, nullptr, t15_check, nullptr, osThreadGetId()};
    spawn(t15_target, nullptr, osPriorityAboveNormal, t15_ts, "T15A");
    osDelay(2);                                          // TA waiting in its first select()
    t15.victim = spawn(victim_main, &t15, osPriorityLow, t15_vs, "T15V");
    sweep_verdict(t15, "no stale rendezvous wakeup: select() never reports C without its data (ALT-vs-ALT)");
    printf("   [T15] bad trials=%lu: PHANTOM (C selected, no data)=%lu, SILENT (X selected, C data written)=%lu\r\n",
           (unsigned long)t15_badtrials, (unsigned long)t15_phantoms, (unsigned long)t15_silents);
}

// ---------------------------------------------------------------------------
// T16 family -- defects found by reading the code in review round 3.
// FAIL = the defect is present. The library is unchanged.
// ---------------------------------------------------------------------------
const char* volatile g_current_test = "-";

#if !BC_ISR_WRITER
// T16s -- SignalChannel putFromISR() to a receiver blocked in input(): it
// must release the receiver (or return false).
BC_SignalChannel* t16s_sig; ThreadSlot t16s_s; volatile int t16s_done = 0; volatile bool t16s_ok = false;
#if BC_OWRV
void t16s_receiver(void*) { csp::Chanin<csp::Signal> in = t16s_sig->reader(); csp::Signal s; in >> s; t16s_done = 1; park(); }
void t16s_isr() { csp::Chanout<csp::Signal> out = t16s_sig->writer(); t16s_ok = out.putFromISR(csp::Signal{}); }
#else
void t16s_receiver(void*) { t16s_sig->getInternal()->input(nullptr); t16s_done = 1; park(); }
void t16s_isr() { t16s_ok = t16s_sig->getInternal()->putFromISR(); }
#endif
void test_T16s() {
    static BC_SignalChannel sig; t16s_sig = &sig; g_current_test = "T16s";
    spawn(t16s_receiver, nullptr, osPriorityNormal, t16s_s, "T16s");
    osDelay(3);                                          // receiver blocked in input()
    g_isr_op = t16s_isr; isr_fire(); osDelay(3); g_isr_op = nullptr;
    osDelay(40);                                         // > 3 of input()'s 100 ms polling slices
    printf("   [T16s] putFromISR returned %d; blocked receiver released=%d\r\n", (int)t16s_ok, t16s_done);
    result("T16s", (t16s_ok == (t16s_done == 1)) ? 1 : 0,
           "signal putFromISR() releases a blocked receiver (or returns false)");
}

#else
void test_T16s() {
    replaced("T16s", "neg_signal_isr.cpp", "signal channels have no ISR writer (data-less rendezvous, task-only)");
}
#endif

#if !BC_ISR_WRITER
// T16n -- KeepNewest rendezvous: output() while a reader waits in an ALT.
// The reader is waiting, so the value must be taken (API: "data is captured
// only if a receiver is already waiting").
csp::SamplingChannel<uint32_t, BufferPolicy::KeepNewest>* t16n_ch; ThreadSlot t16n_s;
volatile int t16n_sel = -2; volatile uint32_t t16n_msg = 0;
void t16n_reader(void*) {
    In in = t16n_ch->reader(); uint32_t msg = T15_SENT;
    csp::RelTimeoutGuard to(csp::Time(50));
    csp::Alternative alt BC_ALTV(BC_G(in, msg), BC_T(to));
    int s = alt.priSelect(); t16n_msg = msg; t16n_sel = s; park();
}
void test_T16n() {
    static csp::SamplingChannel<uint32_t, BufferPolicy::KeepNewest> ch; t16n_ch = &ch; g_current_test = "T16n";
    spawn(t16n_reader, nullptr, osPriorityNormal, t16n_s, "T16n");
    osDelay(3);                                          // reader waits in its ALT
    Out out = ch.writer(); out << 4321u;                 // non-blocking (KeepNewest)
    osDelay(60);                                         // past the reader's timeout
    printf("   [T16n] reader selected %d (0 = channel, 1 = timeout), value %s0x%lx\r\n", t16n_sel,
           t16n_msg == T15_SENT ? "UNCHANGED " : "", (unsigned long)t16n_msg);
    if (t16n_sel == 0 && t16n_msg == T15_SENT) printf("   [T16n] rendezvous reported without data transfer; the value was dropped\r\n");
    result("T16n", (t16n_sel == 0 && t16n_msg == 4321u) ? 1 : 0,
           "KeepNewest rendezvous output() to a waiting ALT reader delivers the value");
}

#else
void test_T16n() {
    replaced("T16n", "neg_rendezvous_policy.cpp",
             "KeepNewest/KeepOldest rendezvous channels are rejected at compile time (use SamplingBufferedChannel<T, 1, P>)");
}
#endif

#if !BC_ISR_WRITER
// T16a (sweep) -- rendezvous putFromISR() vs a plain reader entering input().
// The task path protects AltChanSyncBase with the mutex, putFromISR() with
// BASEPRI; they do not exclude each other. registerWaitingTask() stores
// waiting_in_task, THEN non_alt_in_data_ptr: an ISR in between copies to the
// old (null) pointer and wakes the reader, which returns without the value.
// Victim: plain reader (input()). Aggressor at E1: ISR putFromISR(v); if it
// reports false (no reader yet), the runner delivers a kick value instead.
// Correct: putFromISR() true -> reader got v; false -> reader got the kick.
csp::Channel<uint32_t>* t16a_ch; ThreadSlot t16a_s; Case t16a;
constexpr uint32_t T16A_KICK = 0x0B0B0B0Bu;
volatile uint32_t t16a_val = 0, t16a_got = 0, t16a_bad = 0, t16a_word0_changed = 0;
volatile bool t16a_isr_ok = false, t16a_isr_done = false;
void t16a_reset()  { t16a_val = t16a_val + 1; t16a_got = T15_SENT; t16a_isr_ok = false; t16a_isr_done = false; }
bool t16a_probe()  { return t16a_isr_done; }
void t16a_victim() { static In in = t16a_ch->reader(); uint32_t v = T15_SENT; in >> v; t16a_got = v; }
void t16a_isr()    { Out out = t16a_ch->writer(); uint32_t v = t16a_val; t16a_isr_ok = out.putFromISR(v); t16a_isr_done = true; }
void t16a_aggr()   {
    g_isr_op = t16a_isr; isr_fire(); g_isr_op = nullptr;
    if (!t16a_isr_ok) { Out out = t16a_ch->writer(); out << T16A_KICK; }   // no reader yet: rendezvous normally
}
bool t16a_check()  {
    bool ok = t16a_isr_ok ? (t16a_got == t16a_val) : (t16a_got == T16A_KICK);
    if (!ok) t16a_bad = t16a_bad + 1;
    return ok;
}
void test_T16a() {
    static csp::Channel<uint32_t> ch; t16a_ch = &ch; g_current_test = "T16a";
    t16a = {"T16a", t16a_reset, t16a_probe, t16a_victim, t16a_aggr, nullptr, nullptr, t16a_check, nullptr, osThreadGetId()};
    t16a.victim = spawn(victim_main, &t16a, osPriorityLow, t16a_s, "T16a");
    sweep_verdict(t16a, "rendezvous putFromISR() vs a reader entering input(): value delivered or false returned");
    printf("   [T16a] trials with putFromISR()==true but the value not received: %lu\r\n", (unsigned long)t16a_bad);
    g_isr_op = nullptr;
}

#else
void test_T16a() {
    replaced("T16a", "neg_rendezvous_isr.cpp",
             "no ISR path into rendezvous channels, so no mutex/BASEPRI race (their state is task-only)");
}
#endif

// ---------------------------------------------------------------------------
// T14 -- objects constructed at namespace scope, i.e. during C++ static
// initialisation, BEFORE main() and osKernelInitialize(). The probe records
// what the backend allows then (raw CMSIS-RTOS2 calls, no library code);
// the global channel is then used after the kernel has started.
// ---------------------------------------------------------------------------
struct PreInitProbe {
    osKernelState_t state = osKernelError;
    bool static_sem_ok = false, dynamic_sem_ok = false;
    osSemaphoreId_t static_sem = nullptr;
#if defined(CSP4CMSIS_STATIC_ALLOCATION)
    csp::internal::csp_static_semaphore_storage_t cb;
#endif
    PreInitProbe() {
        state = osKernelGetState();
        osSemaphoreAttr_t a = {};
#if defined(CSP4CMSIS_STATIC_ALLOCATION)
        a.cb_mem = &cb; a.cb_size = sizeof(cb);
#endif
        static_sem = osSemaphoreNew(1, 0, &a);
        static_sem_ok = static_sem != nullptr;
        osSemaphoreId_t d = osSemaphoreNew(1, 0, nullptr);
        dynamic_sem_ok = d != nullptr;
    }
};
PreInitProbe g_preinit;                           // runs before main()
BlockChan<2> g_global_chan;                       // library channel constructed before main()
ThreadSlot t14_s; volatile uint32_t t14_sum = 0;
void t14_writer(void*) { Out out(&g_global_chan); for (uint32_t i = 1; i <= 3; ++i) out << i; park(); }
void test_T14() {
    const char* st = g_preinit.state == osKernelInactive ? "Inactive" : g_preinit.state == osKernelReady ? "Ready" :
                     g_preinit.state == osKernelRunning ? "Running" : "other/error";
    bool static_works_later = false;
    if (g_preinit.static_sem_ok) {                // is the pre-init object usable after start?
        static_works_later = osSemaphoreRelease(g_preinit.static_sem) == osOK &&
                             osSemaphoreAcquire(g_preinit.static_sem, 0) == osOK;
    }
    printf("   [T14] before main(): kernel state=%s; static osSemaphoreNew=%s (usable after start=%s); dynamic osSemaphoreNew=%s\r\n",
           st, g_preinit.static_sem_ok ? "ok" : "NULL", static_works_later ? "yes" : "no", g_preinit.dynamic_sem_ok ? "ok" : "NULL");
    spawn(t14_writer, nullptr, osPriorityNormal, t14_s, "T14w");
    uint32_t x;
    for (int i = 0; i < 3; ++i) { g_global_chan.input(&x); t14_sum = t14_sum + x; }
    printf("   [T14] namespace-scope BufferedChannel after start: received sum=%lu (expect 6)\r\n", (unsigned long)t14_sum);
    result("T14", t14_sum == 6 ? 1 : 0, "a BufferedChannel constructed at namespace scope (before main) works");
}

// ---------------------------------------------------------------------------
// T20..T24 -- timeout guards. 2.0.1: the deadline is fixed when select()
// starts and select() waits with the remaining ticks (no RTOS timer).
// A "never" channel has no writer; tick counts from osKernelGetTickCount().
// ---------------------------------------------------------------------------
uint32_t ticks_now() { return osKernelGetTickCount(); }

// T20 -- accuracy: a timeout of d ticks returns after d or d + 1 ticks
void test_T20() {
    g_current_test = "T20";
    static csp::Channel<uint32_t> never; In in = never.reader(); uint32_t v = 0;
    const uint32_t ds[] = {1, 3, 10}; bool ok = true;
    for (uint32_t d : ds) {
        uint32_t lo = 0xFFFFFFFFu, hi = 0; int bad_sel = 0;
        for (int rep = 0; rep < 5; ++rep) {
            osDelay(1); spin((uint32_t)rep * 997u);           // vary the phase within the tick
            csp::RelTimeoutGuard to{csp::Time(d)};
            csp::Alternative alt BC_ALTV(BC_G(in, v), BC_T(to));
            uint32_t t1 = ticks_now(); int sel = alt.priSelect(); uint32_t dt = ticks_now() - t1;
            if (sel != 1) bad_sel++;
            if (dt < lo) lo = dt;
            if (dt > hi) hi = dt;
        }
        printf("   [T20] timeout %lu ticks: returned after %lu..%lu ticks, wrong guard %d/5\r\n",
               (unsigned long)d, (unsigned long)lo, (unsigned long)hi, bad_sel);
        if (bad_sel || lo < d || hi > d + 1) ok = false;
    }
    result("T20", ok ? 1 : 0, "a timeout of d ticks is selected after d or d+1 ticks");
}

// T21/T23 writer: on F_START waits t21_delay ticks, then writes t21_val (plain rendezvous)
csp::Channel<uint32_t>* t21_ch; ThreadSlot t21_s; osThreadId_t t21_tid = nullptr;
volatile uint32_t t21_delay = 0, t21_val = 0, t21_sent = 0;
void t21_writer(void*) {
    Out out = t21_ch->writer();
    for (;;) {
        osThreadFlagsWait(F_START, osFlagsWaitAny, osWaitForever);
        if (t21_delay) osDelay(t21_delay);
        uint32_t x = t21_val; out << x; t21_sent = t21_sent + 1;
    }
}
void t21_start() {
    static csp::Channel<uint32_t> ch;
    if (t21_tid == nullptr) { t21_ch = &ch; t21_tid = spawn(t21_writer, nullptr, osPriorityAboveNormal, t21_s, "T21w"); osDelay(1); }
}

// T21 -- a timeout racing a channel that becomes ready around the deadline:
//        exactly one guard is selected; a channel item is never lost or doubled
void test_T21() {
    g_current_test = "T21";
    t21_start(); In in = t21_ch->reader();
    constexpr uint32_t D = 10; bool ok = true;
    for (uint32_t k = D - 2; k <= D + 2; ++k) {
        uint32_t c = 0, t = 0;
        for (int rep = 0; rep < 3; ++rep) {
            uint32_t val = 1000u * k + (uint32_t)rep, v = 0, sent0 = t21_sent;
            t21_delay = k; t21_val = val; osThreadFlagsSet(t21_tid, F_START);
            csp::RelTimeoutGuard to{csp::Time(D)};
            csp::Alternative alt BC_ALTV(BC_G(in, v), BC_T(to));
            uint32_t t1 = ticks_now(); int sel = alt.priSelect(); uint32_t dt = ticks_now() - t1;
            if (sel == 0) { c++; if (v != val) ok = false; }
            else {                                            // timed out: the item must still be there
                t++; if (dt < D || dt > D + 1) ok = false;
                uint32_t w = 0; in >> w; if (w != val) ok = false;
            }
            osDelay(2);
            if (t21_sent != sent0 + 1) ok = false;            // exactly one transfer
        }
        printf("   [T21] channel ready after %lu ticks, timeout %lu: channel %lu/3, timeout %lu/3\r\n",
               (unsigned long)k, (unsigned long)D, (unsigned long)c, (unsigned long)t);
        if (k + 2 <= D && c != 3) ok = false;                 // clearly before the deadline: channel
        if (k >= D + 2 && t != 3) ok = false;                 // clearly after: timeout
    }
    result("T21", ok ? 1 : 0, "timeout vs late channel: one guard selected, the item neither lost nor doubled");
}

// T22 -- several timeout guards: the earliest deadline is selected
void test_T22() {
    g_current_test = "T22";
    static csp::Channel<uint32_t> never; In in = never.reader(); uint32_t v = 0;
    csp::RelTimeoutGuard a{csp::Time(30)}, b{csp::Time(5)}, c{csp::Time(15)};
    csp::Alternative alt BC_ALTV(BC_G(in, v), BC_T(a), BC_T(b), BC_T(c));
    uint32_t t1 = ticks_now(); int sel = alt.priSelect(); uint32_t dt = ticks_now() - t1;
    printf("   [T22] timeouts (30, 5, 15): selected guard %d after %lu ticks (expect 2 after 5..6)\r\n", sel, (unsigned long)dt);
    result("T22", (sel == 2 && dt >= 5 && dt <= 6) ? 1 : 0, "several timeout guards: the earliest deadline wins");
}

// T23 -- zero timeout: ready at once, never waits; a ready channel listed first wins
void test_T23() {
    g_current_test = "T23";
    static csp::Channel<uint32_t> never; In nin = never.reader(); uint32_t v = 0;
    csp::RelTimeoutGuard z{csp::Time(0)};
    csp::Alternative alt BC_ALTV(BC_G(nin, v), BC_T(z));
    uint32_t t1 = ticks_now(); int sel = alt.priSelect(); uint32_t dt = ticks_now() - t1;
    t21_start(); In in = t21_ch->reader(); uint32_t w = 0, sent0 = t21_sent;
    t21_delay = 0; t21_val = 4242; osThreadFlagsSet(t21_tid, F_START); osDelay(2);   // writer now waits
    csp::RelTimeoutGuard z2{csp::Time(0)};
    csp::Alternative alt2 BC_ALTV(BC_G(in, w), BC_T(z2));
    int sel2 = alt2.priSelect(); osDelay(2);
    printf("   [T23] no channel: guard %d after %lu ticks; channel ready: guard %d, value %lu, transfers %lu\r\n",
           sel, (unsigned long)dt, sel2, (unsigned long)w, (unsigned long)(t21_sent - sent0));
    result("T23", (sel == 1 && dt == 0 && sel2 == 0 && w == 4242 && t21_sent == sent0 + 1) ? 1 : 0,
           "zero timeout: selected at once; a ready channel listed before it wins");
}

// T24 -- stale wakeups do not postpone a timeout (2.0.0 restarted its timer
//        in every select() round: a wakeup per tick keeps it from expiring,
//        here until the waker stops after 50 ticks)
#if BC_OWRV
ThreadSlot t24_s; osThreadId_t t24_target = nullptr; volatile int t24_stop = 0;
void t24_waker(void*) {                                    // a stale ALT wakeup every tick, at most 50
    for (int i = 0; i < 50 && !t24_stop; ++i) { osDelay(1); if (!t24_stop) (void)osThreadFlagsSet(t24_target, csp::internal::altFlag(0)); }
    park();
}
#endif
void test_T24() {
    g_current_test = "T24";
#if BC_OWRV
    static csp::Channel<uint32_t> never; In in = never.reader(); uint32_t v = 0;
    t24_target = osThreadGetId(); t24_stop = 0;
    spawn(t24_waker, nullptr, osPriorityAboveNormal, t24_s, "T24w");
    csp::RelTimeoutGuard to{csp::Time(10)};
    csp::Alternative alt BC_ALTV(BC_G(in, v), BC_T(to));
    uint32_t t1 = ticks_now(); int sel = alt.priSelect(); uint32_t dt = ticks_now() - t1;
    t24_stop = 1; osDelay(3); (void)osThreadFlagsClear(csp::internal::ALT_FLAG_MASK);
    printf("   [T24] stale wakeup every tick, timeout 10: guard %d after %lu ticks\r\n", sel, (unsigned long)dt);
    result("T24", (sel == 1 && dt >= 10 && dt <= 11) ? 1 : 0, "stale wakeups do not postpone a timeout");
#else
    result("T24", -1, "stale wakeups do not postpone a timeout (needs 2.0's ALT flag layout)");
#endif
}


// ===========================================================================
// T25..T31 -- processes, Run(), time conversion (2.1.0). T25..T27 also run on
// 2.0.x (same API); on 2.0.1, T28..T30 are positive controls and must FAIL.
// ===========================================================================
#if LIB_V2
#if BC_SLEEPFOR_TIME
  #define LIB_2_1 1
#else
  #define LIB_2_1 0
#endif

// A process that records its thread priority and that it ran, then returns
// (or parks, for the tests that inspect a live thread).
constexpr uint32_t PROBE_WORDS = 128, PROBE_TOUCH = 16;
struct ProbeProc : csp::CSProcessStatic<PROBE_WORDS> {
    const char*  nm;
    osPriority_t own;                      // CSP_PRIORITY_UNSPECIFIED: no override
    bool         parks;
    volatile osPriority_t seen = osPriorityError;
    volatile int          ran  = 0;
    ProbeProc(const char* n, osPriority_t p, bool k) : nm(n), own(p), parks(k) {}
    const char* name() const override { return nm; }
    osPriority_t taskPriority() const override { return own; }
    void run() override {
        volatile uint32_t touch[PROBE_TOUCH];      // some stack use, for T27
        for (uint32_t i = 0; i < PROBE_TOUCH; ++i) touch[i] = i;
        (void)touch[PROBE_TOUCH - 1];
        seen = osThreadGetPriority(osThreadGetId());
        ran = ran + 1;
        if (parks) park();
    }
};

// T25 -- TerminatingNetwork: Run() returns after every process returned;
//        taskPriority() overrides the composition priority, the others run at it;
//        without a priority argument the composition priority is osPriorityLow
ProbeProc t25_a("T25a", CSP_PRIORITY_UNSPECIFIED, false), t25_b("T25b", osPriorityLow1, false),
          t25_c("T25c", CSP_PRIORITY_UNSPECIFIED, false),
          t25_d("T25d", CSP_PRIORITY_UNSPECIFIED, false), t25_e("T25e", CSP_PRIORITY_UNSPECIFIED, false);
void test_T25() {
    g_current_test = "T25";
    csp::Run(csp::InParallel(t25_a, t25_b, t25_c), csp::ExecutionMode::TerminatingNetwork, osPriorityBelowNormal);
    bool all = t25_a.ran == 1 && t25_b.ran == 1 && t25_c.ran == 1;
    bool prio = t25_a.seen == osPriorityBelowNormal && t25_b.seen == osPriorityLow1 && t25_c.seen == osPriorityBelowNormal;
    csp::Run(csp::InParallel(t25_d, t25_e), csp::ExecutionMode::TerminatingNetwork);   // default composition priority
    bool dflt = t25_d.ran == 1 && t25_e.ran == 1 && t25_d.seen == osPriorityLow && t25_e.seen == osPriorityLow;
    printf("   [T25] after Run(): ran %d/%d/%d; priorities %d/%d/%d (expect %d/%d/%d); default run: %d/%d at %d/%d (expect %d)\r\n",
           t25_a.ran, t25_b.ran, t25_c.ran, (int)t25_a.seen, (int)t25_b.seen, (int)t25_c.seen,
           (int)osPriorityBelowNormal, (int)osPriorityLow1, (int)osPriorityBelowNormal,
           t25_d.ran, t25_e.ran, (int)t25_d.seen, (int)t25_e.seen, (int)osPriorityLow);
    result("T25", (all && prio && dflt) ? 1 : 0,
           "TerminatingNetwork returns after all processes; taskPriority() overrides the composition priority (default osPriorityLow)");
}

// T26 -- StaticNetwork: Run() returns before any (lower-priority) process ran;
//        each thread is named after name(); all run afterwards
// T27 -- forEachProcess visits every process in declaration order;
//        stackHighWaterMarkWords(): unavailable before Run(), afterwards in (0, N)
//        and reflecting the stack a process used
ProbeProc t26_a("T26a", CSP_PRIORITY_UNSPECIFIED, true), t26_b("T26b", CSP_PRIORITY_UNSPECIFIED, true),
          t26_c("T26c", osPriorityLow2, true);
void test_T26_T27() {
    g_current_test = "T26";
    uint32_t hwm_before = t26_a.stackHighWaterMarkWords();
    auto net = csp::InParallel(t26_a, t26_b, t26_c);
    csp::Run(net, csp::ExecutionMode::StaticNetwork, osPriorityBelowNormal);
    int ran_at_return = t26_a.ran + t26_b.ran + t26_c.ran;
    bool names = t26_a.taskHandle() && t26_b.taskHandle() && t26_c.taskHandle() &&
                 strcmp(osThreadGetName(t26_a.taskHandle()), "T26a") == 0 &&
                 strcmp(osThreadGetName(t26_c.taskHandle()), "T26c") == 0;
    osDelay(5);
    int ran_later = t26_a.ran + t26_b.ran + t26_c.ran;
    printf("   [T26] processes run when Run() returned: %d (expect 0); later: %d (expect 3); thread names %s\r\n",
           ran_at_return, ran_later, names ? "ok" : "WRONG");
    result("T26", (ran_at_return == 0 && ran_later == 3 && names) ? 1 : 0,
           "StaticNetwork returns at once; threads named after name(); all processes run");

    g_current_test = "T27";
    const char* order[3] = {nullptr, nullptr, nullptr}; int n = 0;
    uint32_t hwm[3] = {0, 0, 0};
    net.forEachProcess([&](csp::CSProcess& p) { if (n < 3) { order[n] = p.name(); hwm[n] = p.stackHighWaterMarkWords(); } ++n; });
    bool in_order = n == 3 && order[0] && strcmp(order[0], "T26a") == 0 && strcmp(order[1], "T26b") == 0 && strcmp(order[2], "T26c") == 0;
    bool hwm_ok = hwm_before == CSP_STACK_HWM_UNAVAILABLE;
    for (int i = 0; i < 3; ++i) hwm_ok = hwm_ok && hwm[i] > 0 && hwm[i] <= PROBE_WORDS - PROBE_TOUCH;
    printf("   [T27] forEachProcess: %d processes, order %s; HWM before Run(): %s; after: %lu/%lu/%lu words free of %lu (expect 1..%lu)\r\n",
           n, in_order ? "ok" : "WRONG", hwm_before == CSP_STACK_HWM_UNAVAILABLE ? "unavailable" : "WRONG",
           (unsigned long)hwm[0], (unsigned long)hwm[1], (unsigned long)hwm[2], (unsigned long)PROBE_WORDS, (unsigned long)(PROBE_WORDS - PROBE_TOUCH));
    result("T27", (in_order && hwm_ok) ? 1 : 0,
           "forEachProcess visits every process in order; stackHighWaterMarkWords() unavailable before Run(), then in words");
}

// T28 -- a failed osThreadNew() in Run() is a fatal error (2.0.x: printf and
//        continue, so a TerminatingNetwork caller waited for ever)
// T28 and T29 run their victims one after another in one slot: a victim parks
// in the fatal hook and is terminated (which frees a static TCB at once,
// unlike a thread's own exit) before the slot is reused.
ThreadSlot fatal_slot; ProbeProc t28_p("T28p", CSP_PRIORITY_UNSPECIFIED, false);  // never gets a thread
volatile int t28_after1 = 0, t28_after2 = 0;
void t28_static(void*)      { g_fail_next_thread_new = 1; csp::Run(csp::InParallel(t28_p), csp::ExecutionMode::StaticNetwork);      t28_after1 = 1; park(); }
void t28_terminating(void*) { g_fail_next_thread_new = 1; csp::Run(csp::InParallel(t28_p), csp::ExecutionMode::TerminatingNetwork); t28_after2 = 1; park(); }
void run_victim(osThreadFunc_t fn, const char* name) {
    osThreadId_t t = spawn(fn, nullptr, osPriorityAboveNormal, fatal_slot, name);
    osDelay(3);
    if (t) (void)osThreadTerminate(t);
}
void test_T28() {
    g_current_test = "T28";
    uint32_t f0 = g_fatal_count;
    run_victim(t28_static, "T28s");
    uint32_t f1 = g_fatal_count; const char* m1 = g_fatal_msg;
    run_victim(t28_terminating, "T28t");
    uint32_t f2 = g_fatal_count; const char* m2 = g_fatal_msg;
    bool msg_ok = m1 && m2 && strstr(m1, "osThreadNew") && strstr(m2, "osThreadNew");
    printf("   [T28] StaticNetwork: fatal %lu (\"%s\"), returned %d; TerminatingNetwork: fatal %lu, returned %d\r\n",
           (unsigned long)(f1 - f0), (f1 != f0 && m1) ? m1 : "-", t28_after1, (unsigned long)(f2 - f1), t28_after2);
    result("T28", (f1 == f0 + 1 && f2 == f1 + 1 && msg_ok && !t28_after1 && !t28_after2) ? 1 : 0,
           "a failed osThreadNew() in Run() is a fatal error, in both execution modes");
}

// T29 -- a 17th guard is a fatal error (2.0.x ignored it silently)
volatile int t29_after = 0;
void t29_alt(void*) {
    static csp::Channel<uint32_t> ch; In in = ch.reader(); uint32_t v = 0;
    csp::Alternative alt;
    for (int i = 0; i < 17; ++i) alt.addBinding(in | v);
    t29_after = 1; park();
}
void test_T29() {
    g_current_test = "T29";
    uint32_t f0 = g_fatal_count;
    run_victim(t29_alt, "T29");
    const char* m = g_fatal_msg;
    bool ok = g_fatal_count == f0 + 1 && !t29_after && m && strstr(m, "16 guards");
    printf("   [T29] 17 guards: fatal %lu (\"%s\"), construction completed %d\r\n",
           (unsigned long)(g_fatal_count - f0), (g_fatal_count != f0 && m) ? m : "-", t29_after);
    result("T29", ok ? 1 : 0, "a 17th guard in an Alternative is a fatal error");
}

// T30 -- Seconds()/Milliseconds() round up (2.0.x: down, and ms * freq
//        overflowed 32 bits above ~71.6 min at 1 kHz). Checked as properties,
//        in 64 bits: 0 stays 0; otherwise ticks / freq is never shorter than
//        requested, and at most one tick longer; saturated at 0xFFFFFFFE
//        (osWaitForever is not a duration).
bool t30_ok(uint32_t amount, uint64_t per_second, uint32_t ticks, uint64_t f) {
    const uint64_t want = (uint64_t)amount * f;           // requested duration, in ticks * per_second
    if (amount == 0) return ticks == 0;
    if (ticks == 0xFFFFFFFEu) return want > 0xFFFFFFFDull * per_second;   // saturated: the exact count is >= 0xFFFFFFFE
    return (uint64_t)ticks * per_second >= want && ((uint64_t)ticks - 1) * per_second < want;
}
void test_T30() {
    g_current_test = "T30";
    const uint32_t f = osKernelGetTickFreq();
    const uint32_t big = (uint32_t)(0x100000000ull / f) + 12345u;     // big * f overflows 32 bits
    const uint32_t ms[] = {0u, 1u, 3u, 7u, 999u, 1000u, 1001u, 12345u, big, 0xFFFFFFFFu};
    const uint32_t s[]  = {0u, 1u, 7u, 3600u, 0xFFFFFFFFu};
    int bad = 0;
    for (uint32_t m : ms) {
        uint32_t got = csp::Milliseconds(m).to_ticks();
        if (!t30_ok(m, 1000, got, f)) { ++bad; printf("   [T30] Milliseconds(%lu) = %lu ticks: wrong\r\n", (unsigned long)m, (unsigned long)got); }
    }
    for (uint32_t x : s) {
        uint32_t got = csp::Seconds(x).to_ticks();
        if (!t30_ok(x, 1, got, f)) { ++bad; printf("   [T30] Seconds(%lu) = %lu ticks: wrong\r\n", (unsigned long)x, (unsigned long)got); }
    }
    // Saturation: Seconds(0xFFFFFFFF) exceeds 32 bits of ticks at any frequency above 1 Hz.
    const bool sat = f <= 1u || csp::Seconds(0xFFFFFFFFu).to_ticks() == 0xFFFFFFFEu;
    printf("   [T30] tick frequency %lu Hz: %d of %u conversions wrong; Milliseconds(1) = %lu, Milliseconds(%lu) = %lu, saturation %s\r\n",
           (unsigned long)f, bad, (unsigned)(sizeof(ms) / sizeof(ms[0]) + sizeof(s) / sizeof(s[0])),
           (unsigned long)csp::Milliseconds(1).to_ticks(), (unsigned long)big, (unsigned long)csp::Milliseconds(big).to_ticks(),
           sat ? "ok" : "WRONG");
    result("T30", (bad == 0 && sat) ? 1 : 0, "Seconds()/Milliseconds(): rounded up, 0 stays 0, no 32-bit overflow, saturated");
}

// T31 -- SleepFor(Time) waits the given number of ticks (2.1.0 overload)
void test_T31() {
    g_current_test = "T31";
#if LIB_2_1
    osDelay(1);                                          // start at a tick edge
    uint32_t t1 = ticks_now(); csp::SleepFor(csp::Time(5)); uint32_t d1 = ticks_now() - t1;
    osDelay(1);
    uint32_t t2 = ticks_now(); csp::SleepFor(csp::Time(0)); uint32_t d2 = ticks_now() - t2;
    printf("   [T31] SleepFor(Time(5)): %lu ticks (expect 5..6); SleepFor(Time(0)): %lu ticks (expect 0)\r\n",
           (unsigned long)d1, (unsigned long)d2);
    result("T31", (d1 >= 5 && d1 <= 6 && d2 == 0) ? 1 : 0, "SleepFor(Time) sleeps the given number of ticks");
#else
    result("T31", -1, "SleepFor(Time) (2.1.0 API)");
#endif
}
#else   // 1.x: different process API
void test_T25() { result("T25", -1, "Run(): TerminatingNetwork and priority precedence (2.0 API)"); }
void test_T26_T27() { result("T26", -1, "Run(): StaticNetwork (2.0 API)"); result("T27", -1, "forEachProcess, stackHighWaterMarkWords (2.0 API)"); }
void test_T28() { result("T28", -1, "failed osThreadNew() in Run() (2.0 API)"); }
void test_T29() { result("T29", -1, "17th guard (2.0 API)"); }
void test_T30() { result("T30", -1, "time conversion (2.0 API)"); }
void test_T31() { result("T31", -1, "SleepFor(Time) (2.1.0 API)"); }
#endif

// ---------------------------------------------------------------------------
RunnerSlot runner_slot;
void runner(void*) {
    printf("\r\n=== CSP4CMSIS BufferedChannel regression suite ===\r\n");
    printf("backend: %s; library: %s; %s\r\n", BACKEND_NAME, LIB_NAME, HEAP_MODE);
    isr_init();
    test_T0();  test_T2();  test_T4a(); test_T4b(); test_T4c();
    test_T5();  test_T6();  test_T17(); test_T18(); test_T7a(); test_T8();  test_T9();  test_T10();
    test_T11(); test_T12(); test_T14(); test_T15i(); test_T15s(); test_T16s(); test_T16n();
    test_T1a(); test_T1b(); test_T3();  test_T3i(); test_T13(); test_T13b(); test_T15(); test_T16a();
    test_T20(); test_T21(); test_T22(); test_T23(); test_T24();
    test_T25(); test_T26_T27(); test_T28(); test_T29(); test_T30(); test_T31(); test_T19();
    printf("SUMMARY: PASS=%lu FAIL=%lu SKIP=%lu REPLACED=%lu; heap used=%lu B; runner stack min free=%lu B\r\n",
           (unsigned long)g_pass, (unsigned long)g_fail, (unsigned long)g_skip, (unsigned long)g_replaced, (unsigned long)heap_used(),
           (unsigned long)osThreadGetStackSpace(osThreadGetId()));
    printf("\x04");
    fflush(stdout);
    park();
}

} // namespace

// A fault (e.g. T16a's copy through a null pointer) ends the run with a report
// instead of hanging in the startup file's default handler. Where the project
// already defines HardFault_Handler (STM32CubeMX), BC_HARDFAULT_HANDLER renames
// this one and the project's handler calls it.
#ifndef BC_HARDFAULT_HANDLER
#define BC_HARDFAULT_HANDLER HardFault_Handler
#endif
extern "C" void BC_HARDFAULT_HANDLER(void) {
    printf("!! HardFault during %s: CFSR=0x%08lx HFSR=0x%08lx BFAR=0x%08lx MMFAR=0x%08lx\r\n", g_current_test,
           (unsigned long)SCB->CFSR, (unsigned long)SCB->HFSR, (unsigned long)SCB->BFAR, (unsigned long)SCB->MMFAR);
    printf("SUMMARY: aborted by HardFault\r\n\x04");
    for (;;) { }
}

extern "C" void csp_app_main_init(void) {
    spawn(runner, nullptr, osPriorityHigh, runner_slot, "runner");
}
