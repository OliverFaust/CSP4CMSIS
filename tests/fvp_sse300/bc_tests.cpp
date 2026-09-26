// =============================================================================
// BufferedChannel / ALT verification tests (BUFFERED_CHANNEL_ANALYSIS.md)
//
// Target : Corstone-300 FVP (Cortex-M55), CMSIS-RTOS2 over FreeRTOS 11.3.0,
//          CSP4CMSIS_STATIC_ALLOCATION, CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5.
// Harness: helloworld_sse300 (branch csp4cmsis-wt-tests), which builds this
//          file instead of application.cpp and compiles CSP4CMSIS from this
//          repository's working tree. See tests/fvp_sse300/README.md.
//
// These are TESTS, not library code: they poke at csp::internal classes on
// purpose. Timing notes: the FVP tick runs at ~312.5 Hz (not the configured
// 100 Hz); all delays below are in ticks, and nothing depends on the absolute
// tick length.
//
// Output: one "RESULT <id>: <verdict> ..." line per test, then EOT (0x04),
// which ends the FVP run (mps3_board.uart0.shutdown_on_eot=1).
// =============================================================================
#include "csp/csp4cmsis.h"
#include "cmsis_os2.h"
#include "RTE_Components.h"
#include CMSIS_device_header
#include "FreeRTOS.h"   // xPortGetFreeHeapSize()/StaticTask_t -- test-only
#include "task.h"
#include <cstdio>
#include <cstdint>

using Block      = csp::internal::BufferedChannel<uint32_t, csp::BufferPolicy::Block>;
using KeepNewest = csp::internal::BufferedChannel<uint32_t, csp::BufferPolicy::KeepNewest>;

// ---------------------------------------------------------------------------
// Infrastructure
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t F_START = 0x1u;   // runner -> victim
constexpr uint32_t F_ACK   = 0x2u;   // victim -> runner
constexpr uint32_t T_ACK   = 30u;    // ticks to wait for a victim before calling it hung

template <size_t WORDS>
struct ThreadSlotN {
    alignas(8) uint32_t stack[WORDS];
    StaticTask_t tcb;
};
using ThreadSlot = ThreadSlotN<512>;      // 2 KB: workers
using RunnerSlot = ThreadSlotN<2048>;     // 8 KB: runner (sweep bookkeeping + printf)

template <size_t WORDS>
osThreadId_t spawn(osThreadFunc_t fn, void* arg, osPriority_t prio, ThreadSlotN<WORDS>& s, const char* name) {
    osThreadAttr_t a = {};
    a.name = name; a.priority = prio;
    a.stack_mem = s.stack; a.stack_size = sizeof(s.stack);
    a.cb_mem = &s.tcb;     a.cb_size = sizeof(s.tcb);
    return osThreadNew(fn, arg, &a);
}

void spin(uint32_t k) { for (volatile uint32_t i = 0; i < k; ++i) { } }

template <typename C> void drain(C& ch) { uint32_t x; while (ch.pending()) ch.input(&x); }

void park() { for (;;) osDelay(osWaitForever); }

// ---------------------------------------------------------------------------
// Phase-sweep harness for window races (items 1 and 3).
//
// Per trial (runner = aggressor, higher priority than the victim):
//   runner: reset(); osDelay(1)            -> now just after tick edge E0
//           set START on victim; osDelay(1)-> victim runs, runner wakes at E1
//   victim: spin(k); pre = probe(); victim_op(); ACK
//   runner (preempts the victim at E1 wherever it is): aggressor_op();
//           wait ACK (T_ACK ticks).
// Outcome: EARLY = victim finished its critical code before E1 (pre == false)
//          LATE  = aggressor acted before the victim started (pre == true)
//          BUG   = hang (no ACK although stuck() confirms data/space is
//                  available -> lost wakeup; rescued by one successor op), or
//                  check() failed (lost value)
// Increasing k moves the victim's position at E1 backwards through its code,
// so sweeping k just below the EARLY/LATE boundary covers every instruction
// offset of victim_op's vulnerable window.
// ---------------------------------------------------------------------------
enum Outcome { EARLY = 0, LATE = 1, BUG = 2, ANOMALY = 3 };
#ifndef TRACE_SWEEP
#define TRACE_SWEEP 0
#endif

struct Case {
    const char* id;
    void (*reset)();
    bool (*probe)();
    void (*victim_op)();
    void (*aggressor_op)();
    bool (*stuck)();       // nullptr: op can't hang
    void (*rescue)();
    bool (*check)();       // nullptr: no data-level check
    osThreadId_t victim;
    osThreadId_t runner;
};

volatile uint32_t g_k   = 0;
volatile bool     g_pre = false;
// Victim progress marker, sampled by the aggressor at the instant it runs (E1):
// 0 = spinning, 1 = probing, 2 = inside victim_op, 3 = op done, 4 = ACK sent.
volatile uint32_t g_stage = 0, g_seen = 0;
bool g_trace = TRACE_SWEEP;   // -DTRACE_SWEEP=1 for per-trial tracing
// T3 only: queue content observed by check() in the trial just run.
uint32_t g_content[4]; volatile uint32_t g_ncontent = 0;

void victim_main(void* arg) {
    Case* c = static_cast<Case*>(arg);
    for (;;) {
        osThreadFlagsWait(F_START, osFlagsWaitAny, osWaitForever);
        g_stage = 0;
        spin(g_k);
        g_stage = 1;
        g_pre = c->probe();
        g_stage = 2;
        c->victim_op();
        g_stage = 3;
        osThreadFlagsSet(c->runner, F_ACK);
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

struct BugHit { uint32_t k, seen, n; uint32_t content[4]; };
struct SweepResult { uint32_t kb, lo, hi, n[4], first_bug, last_bug, bug_runs; BugHit bug[32]; uint32_t nbug_k; };

// Binary search the EARLY/LATE boundary, then sweep [kb-below, kb+above].
SweepResult& sweep(Case& c, uint32_t below, uint32_t above) {
    static SweepResult s; s = SweepResult{};
    uint32_t lo = 0, hi = 400000;                  // trial(lo) != LATE, trial(hi) == LATE
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        Outcome o = trial(c, mid);
        if (g_trace) printf("   [%s] bsearch k=%lu -> %d (stage %lu)\r\n", c.id, (unsigned long)mid, (int)o, (unsigned long)g_seen);
        if (o == LATE) hi = mid; else lo = mid;
    }
    s.kb = hi;
    s.lo = (hi > below) ? hi - below : 0;
    s.hi = hi + above;
    s.first_bug = s.last_bug = UINT32_MAX;
    bool in_run = false;
    for (uint32_t k = s.lo; k <= s.hi; ++k) {
        Outcome o = trial(c, k);
        s.n[o]++;
        if (g_trace && o != EARLY)
            printf("   [%s] k=%lu -> %d (stage %lu, BASEPRI=0x%02lx)\r\n", c.id, (unsigned long)k, (int)o,
                   (unsigned long)g_seen, (unsigned long)__get_BASEPRI());
        if (o == BUG) {
            if (s.first_bug == UINT32_MAX) s.first_bug = k;
            s.last_bug = k;
            if (s.nbug_k < 32) {
                BugHit& h = s.bug[s.nbug_k++];
                h.k = k; h.seen = g_seen; h.n = g_ncontent;
                for (uint32_t i = 0; i < 4; ++i) h.content[i] = g_content[i];
            }
            if (!in_run) s.bug_runs++;
        }
        in_run = (o == BUG);
        if (o == ANOMALY) { printf("   [%s] ANOMALY at k=%lu -- sweep aborted\r\n", c.id, (unsigned long)k); break; }
    }
    return s;
}

void report_sweep(const char* id, const char* what, const SweepResult& a, const SweepResult& b) {
    printf("   [%s] boundary kb=%lu, swept k=%lu..%lu: EARLY=%lu LATE=%lu BUG=%lu ANOMALY=%lu\r\n", id,
           (unsigned long)a.kb, (unsigned long)a.lo, (unsigned long)a.hi,
           (unsigned long)a.n[EARLY], (unsigned long)a.n[LATE], (unsigned long)a.n[BUG], (unsigned long)a.n[ANOMALY]);
    if (a.n[BUG]) {
        printf("   [%s] BUG at k=%lu..%lu (%lu value(s) in %lu run(s)); k(victim stage at E1):", id,
               (unsigned long)a.first_bug, (unsigned long)a.last_bug, (unsigned long)a.n[BUG], (unsigned long)a.bug_runs);
        for (uint32_t i = 0; i < a.nbug_k && i < 16; ++i) printf(" %lu(%lu)", (unsigned long)a.bug[i].k, (unsigned long)a.bug[i].seen);
        printf("\r\n");
    }
    bool same = (a.kb == b.kb) && (a.n[BUG] == b.n[BUG]) && (a.first_bug == b.first_bug) && (a.last_bug == b.last_bug);
    printf("   [%s] repeat sweep: kb=%lu BUG=%lu (%lu..%lu) -> %s; k(stage):", id, (unsigned long)b.kb, (unsigned long)b.n[BUG],
           (unsigned long)b.first_bug, (unsigned long)b.last_bug, same ? "same as first sweep" : "differs from first sweep");
    for (uint32_t i = 0; i < b.nbug_k && i < 16; ++i) printf(" %lu(%lu)", (unsigned long)b.bug[i].k, (unsigned long)b.bug[i].seen);
    printf("\r\n");
    // Crude share of a tick in which an aggressor wake-up hits the window: window ~ n_bug
    // spin iterations, tick ~ kb iterations (victim starts shortly after E0).
    if (a.n[BUG] && a.kb)
        printf("   [%s] window ~ %lu/%lu of a tick period (~%lu ppm per tick-aligned wake-up)\r\n", id,
               (unsigned long)a.n[BUG], (unsigned long)a.kb, (unsigned long)((uint64_t)a.n[BUG] * 1000000u / a.kb));
    printf("RESULT %s: %s -- %s\r\n", id, a.n[BUG] ? "CONFIRMED" : "NOT REPRODUCED", what);
}

// ---------------------------------------------------------------------------
// T0 -- control: plain buffered channel + ALT reader works.
// ---------------------------------------------------------------------------
Block* t0_ch; ThreadSlot t0_slot; volatile uint32_t t0_sum = 0, t0_n = 0;
void t0_writer(void*) { for (uint32_t i = 1; i <= 10; ++i) { t0_ch->output(&i); osDelay(1); } park(); }
void test_T0() {
    static Block ch(4); t0_ch = &ch;
    spawn(t0_writer, nullptr, osPriorityNormal, t0_slot, "T0w");
    uint32_t v = 0;
    csp::Alternative alt({ch.getInputGuard(v)});
    for (int i = 0; i < 10; ++i) { alt.priSelect(); t0_sum += v; t0_n++; }
    printf("RESULT T0: %s -- control: 10 values through Block BufferedChannel via ALT, sum=%lu (expect 55)\r\n",
           (t0_n == 10 && t0_sum == 55) ? "PASS" : "FAIL", (unsigned long)t0_sum);
}

// ---------------------------------------------------------------------------
// T1a -- item 1: lost wakeup, BufferedInputGuard::enable() (pending() before register)
// ---------------------------------------------------------------------------
Block* t1a_ch; ThreadSlot t1a_slot; Case t1a;
void t1a_reset() { drain(*t1a_ch); }
bool t1a_probe() { return t1a_ch->pending(); }
void t1a_victim() {
    static uint32_t v;
    static csp::Alternative alt({t1a_ch->getInputGuard(v)});   // built once, in the victim thread
    alt.priSelect();
    drain(*t1a_ch);                                             // consume a rescue "kick", if any
}
void t1a_aggr()   { uint32_t v = 1; t1a_ch->output(&v); }
bool t1a_stuck()  { return t1a_ch->pending(); }                 // data waiting, reader still blocked
void t1a_rescue() { uint32_t kick = 0xFFFFFFFFu; t1a_ch->output(&kick); }  // "successor" message
void test_T1a() {
    static Block ch(4); t1a_ch = &ch;
    t1a = {"T1a", t1a_reset, t1a_probe, t1a_victim, t1a_aggr, t1a_stuck, t1a_rescue, nullptr, nullptr, osThreadGetId()};
    t1a.victim = spawn(victim_main, &t1a, osPriorityLow, t1a_slot, "T1a_rd");
    static SweepResult a, b; a = sweep(t1a, 1500, 50); b = sweep(t1a, 1500, 50);
    report_sweep("T1a", "ALT reader blocks with data queued (input-guard enable race)", a, b);
}

// ---------------------------------------------------------------------------
// T1b -- item 1: lost wakeup, BufferedOutputGuard::enable() (space check before register)
// ---------------------------------------------------------------------------
Block* t1b_ch; ThreadSlot t1b_slot; Case t1b;
void t1b_reset() { drain(*t1b_ch); uint32_t one = 1; t1b_ch->output(&one); }   // capacity 1 -> full
bool t1b_probe() { return t1b_ch->space_available(); }
void t1b_victim() {
    static uint32_t w = 77;
    static csp::Alternative alt({t1b_ch->getOutputGuard(w)});
    alt.priSelect();
}
void t1b_aggr()   { uint32_t x; t1b_ch->input(&x); }            // frees the slot
bool t1b_stuck()  { return t1b_ch->space_available(); }         // space free, writer still blocked
void t1b_rescue() { uint32_t f = 5, x; t1b_ch->output(&f); t1b_ch->input(&x); }  // successor read
void test_T1b() {
    static Block ch(1); t1b_ch = &ch;
    t1b = {"T1b", t1b_reset, t1b_probe, t1b_victim, t1b_aggr, t1b_stuck, t1b_rescue, nullptr, nullptr, osThreadGetId()};
    t1b.victim = spawn(victim_main, &t1b, osPriorityLow, t1b_slot, "T1b_wr");
    static SweepResult a, b; a = sweep(t1b, 1500, 50); b = sweep(t1b, 1500, 50);
    report_sweep("T1b", "ALT writer blocks with space free (output-guard enable race)", a, b);
}

// ---------------------------------------------------------------------------
// T2 -- item 2: wakeUp() (osEventFlagsSet) inside the BASEPRI critical section
//       Replicates _notifyReader()/_notifyWriter() exactly:
//         saved = csp_enter_critical(); alt->wakeUp(bit); csp_exit_critical(saved);
// ---------------------------------------------------------------------------
csp::internal::AltScheduler* t2_sched; ThreadSlot t2_slot; volatile int t2_ran = 0;
void t2_high(void*) {
    osEventFlagsWait(t2_sched->getEventGroupHandle(), 0x1u, osFlagsWaitAny, osWaitForever);
    t2_ran = 1;
    park();
}
void test_T2() {
    static csp::internal::AltScheduler sched; t2_sched = &sched;
    spawn(t2_high, nullptr, osPriorityRealtime, t2_slot, "T2hi");   // above the runner; blocks at once
    osDelay(2);
    uint32_t saved  = csp::internal::csp_enter_critical();
    uint32_t bp_in  = __get_BASEPRI();
    t2_sched->wakeUp(0x1u);
    uint32_t bp_out = __get_BASEPRI();
    int      ran    = t2_ran;                                        // did the woken thread run INSIDE the section?
    csp::internal::csp_exit_critical(saved);
    printf("   [T2] BASEPRI before wakeUp=0x%02lx, after wakeUp (still inside section)=0x%02lx, "
           "higher-priority waiter ran inside section=%d\r\n", (unsigned long)bp_in, (unsigned long)bp_out, ran);
    bool confirmed = (bp_in != 0) && (bp_out == 0) && ran;
    printf("RESULT T2: %s -- osEventFlagsSet() with BASEPRI raised took the thread path and cleared BASEPRI "
           "(critical section ended early; context switch inside it)\r\n", confirmed ? "CONFIRMED" : "NOT REPRODUCED");
}

// ---------------------------------------------------------------------------
// T3 -- item 3: KeepNewest "drop oldest, put newest" is two queue operations
//       (capacity 2, full with {1,2}; victim writes 100, aggressor writes 200;
//        every serial order keeps both 100 and 200)
// ---------------------------------------------------------------------------
KeepNewest* t3_ch; ThreadSlot t3_slot; Case t3; volatile bool t3_aggr_done = false;
void t3_reset() { drain(*t3_ch); uint32_t a = 1, b = 2; t3_ch->output(&a); t3_ch->output(&b); t3_aggr_done = false; }
bool t3_probe() { return t3_aggr_done; }
void t3_victim() { uint32_t v = 100; t3_ch->output(&v); }
void t3_aggr()   { uint32_t v = 200; t3_ch->output(&v); t3_aggr_done = true; }
bool t3_check() {
    bool has100 = false, has200 = false; uint32_t x; g_ncontent = 0;
    while (t3_ch->pending()) { t3_ch->input(&x); if (g_ncontent < 4) g_content[g_ncontent++] = x; has100 |= (x == 100); has200 |= (x == 200); }
    return has100 && has200;
}
void test_T3() {
    static KeepNewest ch(2); t3_ch = &ch;
    t3 = {"T3", t3_reset, t3_probe, t3_victim, t3_aggr, nullptr, nullptr, t3_check, nullptr, osThreadGetId()};
    t3.victim = spawn(victim_main, &t3, osPriorityLow, t3_slot, "T3_wr");
    static SweepResult a, b; a = sweep(t3, 1500, 50); b = sweep(t3, 1500, 50);
    report_sweep("T3", "KeepNewest: concurrent writer steals the freed slot, newest value lost", a, b);
    for (uint32_t i = 0; i < a.nbug_k && i < 6; ++i) {
        printf("   [T3] k=%lu stage=%lu: reader sees", (unsigned long)a.bug[i].k, (unsigned long)a.bug[i].seen);
        for (uint32_t j = 0; j < a.bug[i].n; ++j) printf(" %lu", (unsigned long)a.bug[i].content[j]);
        printf("  (expected 100 and 200 both present)\r\n");
    }
}

// ---------------------------------------------------------------------------
// T4a -- item 4: two ALT writers on a full Block channel; single registration slot
// T4b -- item 4/7: an unrelated ALT's disable() wipes another writer's registration
// T4c -- item 7: shared res_out_guard -- second getOutputGuard() re-targets the first writer
// ---------------------------------------------------------------------------
Block* t4_ch; ThreadSlot t4_sa, t4_sb; volatile int t4_doneA = 0, t4_doneB = 0;
void t4_writerA(void*) { uint32_t a = 10; csp::Alternative alt({t4_ch->getOutputGuard(a)}); alt.priSelect(); t4_doneA = 1; park(); }
void t4_writerB(void*) { osDelay(2); uint32_t b = 20; csp::Alternative alt({t4_ch->getOutputGuard(b)}); alt.priSelect(); t4_doneB = 1; park(); }
void test_T4a() {
    static Block ch(1); t4_ch = &ch; uint32_t x = 1; ch.output(&x);       // full
    spawn(t4_writerA, nullptr, osPriorityNormal, t4_sa, "T4a_A");
    spawn(t4_writerB, nullptr, osPriorityNormal, t4_sb, "T4a_B");
    osDelay(6);                                                           // A and B both blocked in ALT
    uint32_t v1, v2; ch.input(&v1); osDelay(3); ch.input(&v2); osDelay(10);
    bool confirmed = (t4_doneB == 1) && (t4_doneA == 0) && ch.space_available() && !ch.pending();
    printf("   [T4a] reader got %lu then %lu; writer A done=%d, writer B done=%d, space free=%d\r\n",
           (unsigned long)v1, (unsigned long)v2, t4_doneA, t4_doneB, (int)ch.space_available());
    printf("RESULT T4a: %s -- second ALT writer overwrote the single alt_writer slot; first writer never woken\r\n",
           confirmed ? "CONFIRMED" : "NOT REPRODUCED");
}

Block* t4b_ch; ThreadSlot t4b_sa; volatile int t4b_doneA = 0;
void t4b_writerA(void*) { uint32_t a = 10; csp::Alternative alt({t4b_ch->getOutputGuard(a)}); alt.priSelect(); t4b_doneA = 1; park(); }
void test_T4b() {
    static Block ch(1); t4b_ch = &ch; uint32_t x = 1; ch.output(&x);     // full
    spawn(t4b_writerA, nullptr, osPriorityNormal, t4b_sa, "T4b_A");
    osDelay(3);                                                           // A registered, blocked
    {   // unrelated writer process: ALT {instant timeout, output} -- timeout wins, output guard never enabled
        uint32_t b = 20;
        static csp::RelTimeoutGuard instant(csp::Time(0));
        csp::Alternative alt({instant.internal_guard_ptr, ch.getOutputGuard(b)});
        int sel = alt.priSelect();
        printf("   [T4b] unrelated ALT selected guard %d (0 = instant timeout)\r\n", sel);
    }
    uint32_t v; ch.input(&v); osDelay(10);                                // frees the slot
    bool confirmed = (t4b_doneA == 0) && ch.space_available();
    printf("RESULT T4b: %s -- disable() of a never-enabled output guard cleared writer A's registration; "
           "A not woken although space is free\r\n", confirmed ? "CONFIRMED" : "NOT REPRODUCED");
}

Block* t4c_ch; ThreadSlot t4c_sa; volatile int t4c_doneA = 0;
void t4c_writerA(void*) { uint32_t a = 10; csp::Alternative alt({t4c_ch->getOutputGuard(a)}); alt.priSelect(); t4c_doneA = 1; park(); }
void test_T4c() {
    static Block ch(1); t4c_ch = &ch; uint32_t x = 1; ch.output(&x);     // full
    spawn(t4c_writerA, nullptr, osPriorityNormal, t4c_sa, "T4c_A");
    osDelay(3);                                                           // A registered, blocked, target = &a (10)
    static uint32_t b = 20;
    (void)ch.getOutputGuard(b);   // another writer merely *builds* its ALT guard (e.g. Alternative ctor)
    uint32_t v1, v2; ch.input(&v1); osDelay(3);                           // wakes A (still registered)
    ch.input(&v2);
    printf("   [T4c] writer A (sending 10) done=%d; reader received %lu\r\n", t4c_doneA, (unsigned long)v2);
    printf("RESULT T4c: %s -- shared res_out_guard: A's ALT wrote the other writer's value\r\n",
           (t4c_doneA == 1 && v2 == 20) ? "CONFIRMED" : "NOT REPRODUCED");
}

// ---------------------------------------------------------------------------
// T5 -- item 5: osMessageQueueNew() failure is not checked
// ---------------------------------------------------------------------------
void test_T5() {
    size_t free0 = xPortGetFreeHeapSize();
    static Block big(20000);                          // 80 KB of messages > 32 KB FreeRTOS heap
    uint32_t dest = 0xDEADBEEFu, src = 42;
    uint32_t t0 = osKernelGetTickCount();
    big.input(&dest);                                 // Block policy: "waits forever" for data
    big.output(&src);                                 // Block policy: "waits forever" for space
    uint32_t dt = osKernelGetTickCount() - t0;
    printf("   [T5] heap free=%u, queue handle=%p; input() returned after %lu ticks with dest=0x%08lx; "
           "output() returned; pending()=%d space_available()=%d\r\n", (unsigned)free0, (void*)big.getQueueHandle(),
           (unsigned long)dt, (unsigned long)dest, (int)big.pending(), (int)big.space_available());
    bool confirmed = big.getQueueHandle() == nullptr && dest == 0xDEADBEEFu;
    printf("RESULT T5: %s -- NULL queue handle; blocking input() returns at once without writing dest, "
           "output() silently drops\r\n", confirmed ? "CONFIRMED" : "NOT REPRODUCED");
}

// ---------------------------------------------------------------------------
// T6 -- item 6: RelTimeoutGuard/TimerGuard allocate from the RTOS heap per construction
// ---------------------------------------------------------------------------
void test_T6() {
    size_t f0 = xPortGetFreeHeapSize(), f_in = 0, f_alt = 0;
    {
        csp::RelTimeoutGuard g(csp::Time(5));
        f_in = xPortGetFreeHeapSize();
    }
    size_t f1 = xPortGetFreeHeapSize();
    {
        uint32_t v; static Block ch(1);
        size_t before = xPortGetFreeHeapSize();
        csp::Alternative alt({ch.getInputGuard(v)});   // AltScheduler -> osEventFlagsNew (static cb_mem)
        f_alt = before - xPortGetFreeHeapSize();
    }
    uint32_t allocs = 0;
    for (int i = 0; i < 1000; ++i) {                    // "timeout per loop iteration" pattern
        size_t b = xPortGetFreeHeapSize();
        csp::RelTimeoutGuard g(csp::Time(5));
        if (xPortGetFreeHeapSize() < b) allocs++;
    }
    printf("   [T6] RelTimeoutGuard: %u bytes taken from heap while alive, %u after destruction; "
           "Alternative (STATIC_ALLOCATION): %u bytes; 1000 loop constructions -> %lu heap allocations\r\n",
           (unsigned)(f0 - f_in), (unsigned)(f0 - f1), (unsigned)f_alt, (unsigned long)allocs);
    printf("RESULT T6: %s -- each RelTimeoutGuard construction allocates an osTimer from the RTOS heap, "
           "even with CSP4CMSIS_STATIC_ALLOCATION\r\n", (f0 > f_in && allocs == 1000) ? "CONFIRMED" : "NOT REPRODUCED");
}

// ---------------------------------------------------------------------------
// T7a -- item 7: BufferedInputGuard::activate() does not notify an ALT writer
//        (with control: the same sequence using plain input())
// ---------------------------------------------------------------------------
Block* t7_ch; ThreadSlot t7_s1, t7_s2; volatile int t7_done = 0;
void t7_writer(void*) { uint32_t w = 10; csp::Alternative alt({t7_ch->getOutputGuard(w)}); alt.priSelect(); t7_done = 1; park(); }
bool t7_run(bool reader_uses_alt, ThreadSlot& slot) {
    static Block chA(1), chB(1);
    Block& ch = reader_uses_alt ? chA : chB; t7_ch = &ch; t7_done = 0;
    uint32_t x = 1; ch.output(&x);                         // full
    spawn(t7_writer, nullptr, osPriorityNormal, slot, reader_uses_alt ? "T7_alt" : "T7_ctl");
    osDelay(3);                                            // writer registered, blocked
    uint32_t r = 0;
    if (reader_uses_alt) { csp::Alternative alt({ch.getInputGuard(r)}); alt.priSelect(); }
    else                 { ch.input(&r); }
    osDelay(10);
    return t7_done == 1;
}
void test_T7a() {
    bool ctl = t7_run(false, t7_s1);
    bool alt = t7_run(true,  t7_s2);
    printf("   [T7a] control (reader uses input()): writer woken=%d; reader uses ALT: writer woken=%d\r\n", ctl, alt);
    printf("RESULT T7a: %s -- a read via ALT never wakes a writer blocked in ALT on the same channel\r\n",
           (ctl && !alt) ? "CONFIRMED" : "NOT REPRODUCED");
}

// ---------------------------------------------------------------------------
RunnerSlot runner_slot;
void runner(void*) {
    printf("\r\n=== CSP4CMSIS BufferedChannel verification (FVP, tick ~312.5 Hz) ===\r\n");
    test_T0();
    test_T2();
    test_T4a();
    test_T4b();
    test_T4c();
    test_T5();
    test_T6();
    test_T7a();
    test_T1a();
    test_T1b();
    test_T3();
    printf("=== done; heap free=%u min_ever=%u; runner stack min free=%lu of %u bytes ===\r\n",
           (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize(),
           (unsigned long)osThreadGetStackSpace(osThreadGetId()), (unsigned)sizeof(runner_slot.stack));
    printf("\x04");
    fflush(stdout);   // stdout is buffered: without this the EOT never reaches the UART
    park();
}

} // namespace

extern "C" void csp_app_main_init(void) {
    spawn(runner, nullptr, osPriorityHigh, runner_slot, "runner");
}
