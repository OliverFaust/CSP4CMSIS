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
// =============================================================================
#include "csp/csp4cmsis.h"
#include "cmsis_os2.h"
#include "RTE_Components.h"
#include CMSIS_device_header
#include <cstdio>
#include <cstdint>
#include <cstring>

#if defined(RTE_CMSIS_RTOS2_FreeRTOS)
  #include "FreeRTOS.h"
  #define BACKEND_NAME "FreeRTOS 11.3.0 (CMSIS-RTOS2 adapter)"
  static uint32_t heap_used() { return (uint32_t)(configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize()); }
#elif defined(RTE_CMSIS_RTOS2_RTX5)
  #include "rtx_os.h"
  #define BACKEND_NAME "Keil RTX5 5.9.1"
  // RTX5 dynamic memory pool: mem_head_t { uint32_t size; uint32_t used; } (rtx_memory.c)
  static uint32_t heap_used() { return static_cast<const uint32_t*>(osRtxInfo.mem.common)[1]; }
  extern "C" uint32_t osRtxErrorNotify(uint32_t code, void* object_id) {
      printf("!! RTX5 osRtxErrorNotify(code=%lu, object=%p) -- halting\r\n", (unsigned long)code, object_id);
      for (;;) { }
  }
#else
  #error "unknown CMSIS-RTOS2 backend"
#endif

// ---------------------------------------------------------------------------
// Library-version shim
// ---------------------------------------------------------------------------
#if defined(CSP4CMSIS_BUFFERED_CHANNEL_API) && (CSP4CMSIS_BUFFERED_CHANNEL_API >= 2)
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
}

// ---------------------------------------------------------------------------
// Infrastructure
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t F_START = 0x10000000u;   // runner -> victim (outside CSP4CMSIS's reserved bits)
constexpr uint32_t F_ACK   = 0x20000000u;   // victim -> runner
constexpr uint32_t T_ACK   = 30u;           // ticks before a victim counts as hung

template <size_t WORDS>
struct ThreadSlotN {
    alignas(8) uint32_t stack[WORDS];
    csp::internal::csp_static_thread_storage_t tcb;
};
using ThreadSlot = ThreadSlotN<256>;      // 1 KB workers (T5 needs a 32 KB static channel in RAM)
using RunnerSlot = ThreadSlotN<2048>;     // 8 KB runner

template <size_t WORDS>
osThreadId_t spawn(osThreadFunc_t fn, void* arg, osPriority_t prio, ThreadSlotN<WORDS>& s, const char* name) {
    osThreadAttr_t a = {};
    a.name = name; a.priority = prio;
    a.stack_mem = s.stack; a.stack_size = sizeof(s.stack);
    a.cb_mem = &s.tcb;     a.cb_size = sizeof(s.tcb);
    return osThreadNew(fn, arg, &a);
}

void spin(uint32_t k) { for (volatile uint32_t i = 0; i < k; ++i) { } }
void park() { for (;;) osDelay(osWaitForever); }
template <typename C> void drain(C& ch) { uint32_t x; while (ch.pending()) ch.input(&x); }

uint32_t g_pass = 0, g_fail = 0, g_skip = 0;
void result(const char* id, int verdict /*1 pass, 0 fail, -1 skip*/, const char* what) {
    const char* v = verdict > 0 ? "PASS" : (verdict == 0 ? "FAIL" : "SKIP");
    if (verdict > 0) g_pass++; else if (verdict == 0) g_fail++; else g_skip++;
    printf("RESULT %s: %s -- %s\r\n", id, v, what);
}

// ---------------------------------------------------------------------------
// Software interrupt: I2S_IRQn is unused on the FVP; the aggressor pends it
// and the handler runs the ISR-side operation of the current test.
// Priority 6 (of 0..7) is below CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5,
// i.e. masked by CSP critical sections, as required for putFromISR().
// ---------------------------------------------------------------------------
void (* volatile g_isr_op)() = nullptr;
void isr_init() { NVIC_SetPriority(I2S_IRQn, 6); NVIC_ClearPendingIRQ(I2S_IRQn); NVIC_EnableIRQ(I2S_IRQn); }
void isr_fire() { NVIC_SetPendingIRQ(I2S_IRQn); __DSB(); __ISB(); }
} // namespace
extern "C" void I2S_Handler(void) { if (g_isr_op) g_isr_op(); }
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
    uint32_t lo = 0, hi = 400000;
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
    a = sweep(c, 1500, 1000); b = sweep(c, 1500, 1000);
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
    csp::Alternative alt({in.getGuard(v)});
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
    static csp::Alternative alt({in.getGuard(v)});
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
    static csp::Alternative alt({out.getGuard(w)});
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
    In in(t2_b); uint32_t v; csp::Alternative alt({in.getGuard(v)});
    for (int i = 0; i < 2; ++i) { alt.priSelect(); t2_got = t2_got + v; }
    park();
}
void t2_alt_writer(void*) {            // ALT writer on the (full) Block channel
    Out out(t2_b); uint32_t w = 7; csp::Alternative alt({out.getGuard(w)});
    alt.priSelect(); park();
}
void t2_newest_reader(void*) {         // ALT reader on KeepNewest, 2 rounds
    In in(t2_n); uint32_t v; csp::Alternative alt({in.getGuard(v)});
    for (int i = 0; i < 2; ++i) { alt.priSelect(); t2_got = t2_got + v; }
    park();
}
void t2_rv_reader(void*) { uint32_t v; csp::Chanin<uint32_t> in = t2_r->reader(); in >> v; t2_got = t2_got + v; park(); }
void t2_isr_buffered() { uint32_t v = 1000; t2_b->putFromISR(v); }
void t2_isr_newest()   { uint32_t v = 2000; t2_n->putFromISR(v); }
void t2_isr_rv()       { uint32_t v = 3000; Out out = t2_r->writer(); out.putFromISR(v); }
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
    // (4) rendezvous putFromISR() to a blocked reader
    spawn(t2_rv_reader, nullptr, osPriorityAboveNormal, t2_s4, "T2rv"); osDelay(2);
    g_isr_op = t2_isr_rv; isr_fire(); osDelay(2);
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
void t3_isr()    { uint32_t v = 200; t3_ch->putFromISR(v); t3_aggr_done = true; }
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
    In in(t13_ch); uint32_t v = 0; csp::Alternative alt({in.getGuard(v)});
    for (;;) { osThreadFlagsWait(F_GO, osFlagsWaitAny, osWaitForever); alt.priSelect(); osThreadFlagsSet(t13.runner, F_ACK2); }
}
void t13_writer_aggr(void*) {                     // high-priority ALT writer
    Out out(t13_ch); uint32_t w = 9; csp::Alternative alt({out.getGuard(w)});
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
void t4_writerA(void*) { Out out(t4_ch); uint32_t a = 10; csp::Alternative alt({out.getGuard(a)}); alt.priSelect(); t4_doneA = 1; park(); }
void t4_writerB(void*) { osDelay(2); Out out(t4_ch); uint32_t b = 20; csp::Alternative alt({out.getGuard(b)}); alt.priSelect(); t4_doneB = 1; park(); }
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
void t4b_writerA(void*) { Out out(t4b_ch); uint32_t a = 10; csp::Alternative alt({out.getGuard(a)}); alt.priSelect(); t4b_doneA = 1; park(); }
void test_T4b() {
    static BlockChan<1> ch; t4b_ch = &ch; uint32_t x = 1; ch.output(&x);
    spawn(t4b_writerA, nullptr, osPriorityNormal, t4b_sa, "T4bA");
    osDelay(3);
    {   // unrelated ALT: timeout guard ready first -> output guard never enabled
        static Out outB(&ch); uint32_t b = 20;
        static csp::RelTimeoutGuard instant(csp::Time(0));
        csp::Alternative alt({instant.internal_guard_ptr, outB.getGuard(b)});
        (void)alt.priSelect();
    }
    uint32_t v; ch.input(&v); osDelay(5);
    result("T4b", t4b_doneA == 1 ? 1 : 0, "an ALT that never enabled a guard does not cancel another writer's registration");
}

BlockChan<1>* t4c_ch; ThreadSlot t4c_sa; volatile int t4c_doneA = 0;
void t4c_writerA(void*) { Out out(t4c_ch); uint32_t a = 10; csp::Alternative alt({out.getGuard(a)}); alt.priSelect(); t4c_doneA = 1; park(); }
void test_T4c() {
    static BlockChan<1> ch; t4c_ch = &ch; uint32_t x = 1; ch.output(&x);
    spawn(t4c_writerA, nullptr, osPriorityNormal, t4c_sa, "T4cA");
    osDelay(3);
    static Out outB(&ch); static uint32_t b = 20;
    (void)outB.getGuard(b);                        // another writer builds its guard
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
// T6 -- no RTOS heap for timeout guards / Alternatives (STATIC_ALLOCATION)
// ---------------------------------------------------------------------------
void test_T6() {
    uint32_t u0 = heap_used(), u_in = 0, u_alt = 0, allocs = 0;
    { csp::RelTimeoutGuard g(csp::Time(5)); u_in = heap_used(); }
    { static BlockChan<1> ch; static In in(&ch); uint32_t v; uint32_t b = heap_used();
      csp::Alternative alt({in.getGuard(v)}); u_alt = heap_used() - b; }
    for (int i = 0; i < 200; ++i) { uint32_t b = heap_used(); csp::RelTimeoutGuard g(csp::Time(5)); if (heap_used() > b) allocs++; }
    printf("   [T6] RelTimeoutGuard: %lu B heap while alive; Alternative: %lu B; 200 loop constructions -> %lu allocations\r\n",
           (unsigned long)(u_in - u0), (unsigned long)u_alt, (unsigned long)allocs);
    result("T6", (u_in == u0 && u_alt == 0 && allocs == 0) ? 1 : 0, "RelTimeoutGuard and Alternative use no RTOS heap");
}

// ---------------------------------------------------------------------------
// T7a -- a read via ALT wakes a writer blocked in ALT (control: plain input)
// ---------------------------------------------------------------------------
BlockChan<1>* t7_ch; ThreadSlot t7_s1, t7_s2; volatile int t7_done = 0;
void t7_writer(void*) { Out out(t7_ch); uint32_t w = 10; csp::Alternative alt({out.getGuard(w)}); alt.priSelect(); t7_done = 1; park(); }
bool t7_run(bool reader_uses_alt, ThreadSlot& slot) {
    static BlockChan<1> chA, chB;
    BlockChan<1>& ch = reader_uses_alt ? chA : chB; t7_ch = &ch; t7_done = 0;
    uint32_t x = 1; ch.output(&x);
    spawn(t7_writer, nullptr, osPriorityNormal, slot, reader_uses_alt ? "T7alt" : "T7ctl");
    osDelay(3);
    uint32_t r = 0;
    if (reader_uses_alt) { static In in(&chA); csp::Alternative alt({in.getGuard(r)}); alt.priSelect(); }
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
    In in(t9_ch); uint32_t v = 0; csp::Alternative alt({in.getGuard(v)});
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
void t10_r1(void*) { In in(t10_ch); uint32_t v; csp::Alternative alt({in.getGuard(v)}); alt.priSelect(); t10_done1 = 1; park(); }
void t10_r2(void*) { osDelay(2); In in(t10_ch); uint32_t v; csp::Alternative alt({in.getGuard(v)}); alt.priSelect(); t10_done2 = 1; park(); }
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
    csp::Alternative alt({out.getGuard(w), to.internal_guard_ptr});
    t11_wsel = alt.priSelect(); t11_wdone = 1; park();
}
void test_T11() {
    static csp::Channel<uint32_t> ch; t11_ch = &ch;
    spawn(t11_writer, nullptr, osPriorityNormal, t11_s, "T11w");
    osDelay(3);                                          // writer is waiting in its ALT
    In in = ch.reader(); uint32_t r = 0;
    csp::RelTimeoutGuard to(csp::Time(50));
    csp::Alternative alt({in.getGuard(r), to.internal_guard_ptr});
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

// T15i -- rendezvous putFromISR() to a waiting ALT reader: the ISR reports
// success, the reader's select() returns the channel guard. Correct: the
// reader has the ISR's value (or putFromISR() returns false and the reader
// times out).
csp::Channel<uint32_t>* t15i_ch; ThreadSlot t15i_s;
volatile int t15i_sel = -2; volatile uint32_t t15i_msg = 0; volatile bool t15i_isr_ok = false;
void t15i_reader(void*) {
    In in = t15i_ch->reader(); uint32_t msg = T15_SENT;
    csp::RelTimeoutGuard to(csp::Time(50));
    csp::Alternative alt({in.getGuard(msg), to.internal_guard_ptr});
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

// T15s -- SignalChannel ALT receiver {X (buffered, index 0), signal (index 1)}.
// X becomes ready and a sender signals before the receiver runs; the receiver
// takes X (lower index). The signal must still be received by the next
// select(), and the sender must complete.
BlockChan<1>* t15s_x; csp::SignalChannel<>* t15s_sig; ThreadSlot t15s_rs, t15s_ss;
osThreadId_t t15s_sender_tid; volatile int t15s_sel1 = -2, t15s_sel2 = -2, t15s_sent = 0;
void t15s_receiver(void*) {
    In xin(t15s_x); uint32_t xv = 0;
    auto* sig = t15s_sig->getInternal();
    csp::Alternative alt1({xin.getGuard(xv), sig->getInputGuard()});
    t15s_sel1 = alt1.priSelect();
    csp::RelTimeoutGuard to(csp::Time(50));
    csp::Alternative alt2({xin.getGuard(xv), sig->getInputGuard(), to.internal_guard_ptr});
    t15s_sel2 = alt2.priSelect();
    park();
}
void t15s_sender(void*) {
    osThreadFlagsWait(F_GO, osFlagsWaitAny, osWaitForever);
    t15s_sig->getInternal()->output(nullptr);
    t15s_sent = 1; park();
}
void test_T15s() {
    static BlockChan<1> x; static csp::SignalChannel<> sig; t15s_x = &x; t15s_sig = &sig;
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
    csp::Alternative alt({in.getGuard(msg), xin.getGuard(xv)});
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
    static csp::Alternative alt({out.getGuard(w)});
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
// T14 -- objects constructed at namespace scope, i.e. during C++ static
// initialisation, BEFORE main() and osKernelInitialize(). The probe records
// what the backend allows then (raw CMSIS-RTOS2 calls, no library code);
// the global channel is then used after the kernel has started.
// ---------------------------------------------------------------------------
struct PreInitProbe {
    osKernelState_t state = osKernelError;
    bool static_sem_ok = false, dynamic_sem_ok = false;
    osSemaphoreId_t static_sem = nullptr;
    csp::internal::csp_static_semaphore_storage_t cb;
    PreInitProbe() {
        state = osKernelGetState();
        osSemaphoreAttr_t a = {}; a.cb_mem = &cb; a.cb_size = sizeof(cb);
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
RunnerSlot runner_slot;
void runner(void*) {
    printf("\r\n=== CSP4CMSIS BufferedChannel regression suite ===\r\n");
    printf("backend: %s; library: %s\r\n", BACKEND_NAME, LIB_NAME);
    isr_init();
    test_T0();  test_T2();  test_T4a(); test_T4b(); test_T4c();
    test_T5();  test_T6();  test_T7a(); test_T8();  test_T9();  test_T10();
    test_T11(); test_T12(); test_T14(); test_T15i(); test_T15s();
    test_T1a(); test_T1b(); test_T3();  test_T3i(); test_T13(); test_T13b(); test_T15();
    printf("SUMMARY: PASS=%lu FAIL=%lu SKIP=%lu; heap used=%lu B; runner stack min free=%lu B\r\n",
           (unsigned long)g_pass, (unsigned long)g_fail, (unsigned long)g_skip, (unsigned long)heap_used(),
           (unsigned long)osThreadGetStackSpace(osThreadGetId()));
    printf("\x04");
    fflush(stdout);
    park();
}

} // namespace

extern "C" void csp_app_main_init(void) {
    spawn(runner, nullptr, osPriorityHigh, runner_slot, "runner");
}
