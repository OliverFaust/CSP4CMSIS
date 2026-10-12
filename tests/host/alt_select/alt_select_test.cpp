// Host unit test: winner selection in AltScheduler::select() (3.0.1).
//
// 3.0.0 picked the winner of phase 4 with a loop over the re-verified guards;
// 3.0.1 builds ready_mask in phase 3 and takes order[__CLZ(ready_mask)]. This
// test runs the library's select() (csp4cmsis/src/alternative.cpp, compiled
// for the host against stand-ins for the RTOS and CMSIS-Core) with scripted
// guards, and compares its result with the 3.0.0 loop:
//   - a table of hand-checked cases (kTable), and
//   - every case: 1..16 guards, every offset, every number of guards enabled
//     in the round (phase 1 stops at a guard that is ready at enable(), or
//     enables all and waits), and every ready mask over the enabled guards:
//     none, one or several ready.
// A round without a ready guard must start a new round (the 3.0.0 loop gives
// -1); the scripted second round then makes the first guard in fairness order
// ready. Claimed rounds (a partner committed to one guard) must select the
// claimed guard whatever the mask. Run with ./run.sh.

#include "csp/alt.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

uint32_t host_basepri = 0;
static int g_round = 0;                // rounds of the current select()

// ---------------------------------------------------------------------------
// CMSIS-RTOS2 stand-ins: what select() calls. No other thread exists, so a
// wait returns at once (as a wakeup that select() must re-verify). select()
// clears its thread flags once per round, first thing: that counts rounds.
// ---------------------------------------------------------------------------
extern "C" {
osThreadId_t osThreadGetId(void) { return reinterpret_cast<osThreadId_t>(1); }
uint32_t osKernelGetTickCount(void) { return 0; }
uint32_t osKernelGetTickFreq(void) { return 1000; }
uint32_t osThreadFlagsClear(uint32_t) { ++g_round; return 0; }
uint32_t osThreadFlagsSet(osThreadId_t, uint32_t flags) { return flags; }
uint32_t osThreadFlagsWait(uint32_t flags, uint32_t, uint32_t) { return flags; }
void csp4cmsis_fatal_error(const char* message) {
    std::printf("FAIL: fatal error: %s\n", message);
    std::exit(1);
}
}

using csp::internal::AltScheduler;
using csp::internal::Guard;
using csp::internal::ALT_MAX_GUARDS;

// ---------------------------------------------------------------------------
// The 3.0.0 phase-4 selection, unclaimed case (alternative.cpp at v3.0.0):
//   for (size_t k = 0; k < enabled; ++k)
//       if (ready[k]) { selected = (int)order[k]; break; }
// ---------------------------------------------------------------------------
static int oldSelect(const size_t* order, size_t enabled, const bool* ready) {
    int selected = -1;
    for (size_t k = 0; k < enabled; ++k)
        if (ready[k]) { selected = (int)order[k]; break; }
    return selected;
}

// ---------------------------------------------------------------------------
// Scripted guards. A case describes round 1 in fairness positions k
// (order[k] = (offset + k) % n):
//   enabled  guards enabled in round 1: phase 1 stops at position
//            enabled - 1 if `stop`, else it enables all n and waits
//   ready    bit k: order[k]'s disable() reports ready (re-verification)
//   claim    position whose enable() claims the ALT for its guard, as a
//            partner's claim does (-1: none); it is the stopping guard
// Round 2 (reached only if round 1 selects nothing): order[0] is ready at
// enable() and at disable().
// ---------------------------------------------------------------------------
struct Case {
    size_t   n;
    size_t   offset;
    size_t   enabled;
    bool     stop;
    uint32_t ready;
    int      claim;
};

struct Run {
    Case   c{};
    size_t activates = 0;
    std::vector<size_t> disabled;      // guard indices disabled in round 1, in call order
};
static Run g_run;

static size_t position(size_t idx) {   // fairness position of guard idx
    return (idx + g_run.c.n - g_run.c.offset) % g_run.c.n;
}

class ScriptGuard : public Guard {
public:
    size_t idx = 0;
    bool enable(AltScheduler* alt, uint32_t flag) override {
        size_t k = position(idx);
        if (g_round >= 2) return k == 0;
        if (g_run.c.claim >= 0 && k == (size_t)g_run.c.claim) {
            alt->claimLocked(flag);
            return true;
        }
        return g_run.c.stop && k == g_run.c.enabled - 1;
    }
    bool disable() override {
        size_t k = position(idx);
        if (g_round >= 2) return k == 0;
        g_run.disabled.push_back(idx);
        return (g_run.c.ready >> k) & 1U;
    }
    bool activate() override { ++g_run.activates; return true; }
};

static ScriptGuard g_guards[ALT_MAX_GUARDS];
static Guard*      g_ptrs[ALT_MAX_GUARDS];

struct Counts { unsigned long cases = 0, none = 0, one = 0, several = 0, claimed = 0; };

// Runs select() on case c. round1 = the winner of round 1 (-1: none, so a
// new round must follow); expected = what select() must return.
static bool check(const Case& c, int round1, int expected, Counts& counts) {
    AltScheduler alt;
    g_run = Run{};
    g_run.c = c;
    g_round = 0;
    for (size_t i = 0; i < c.n; ++i) { g_guards[i].idx = i; g_ptrs[i] = &g_guards[i]; }

    unsigned int got = alt.select(g_ptrs, c.n, c.offset);

    bool ok = (int)got == expected && g_run.activates == 1 && g_round == (round1 < 0 ? 2 : 1);
    // Phase 3 disabled exactly the guards enabled in round 1, in fairness order.
    if (g_run.disabled.size() != c.enabled) ok = false;
    for (size_t k = 0; ok && k < g_run.disabled.size(); ++k)
        if (g_run.disabled[k] != (c.offset + k) % c.n) ok = false;

    ++counts.cases;
    if (c.claim >= 0) ++counts.claimed;
    else if (c.ready == 0U) ++counts.none;
    else if ((c.ready & (c.ready - 1U)) == 0U) ++counts.one;
    else ++counts.several;

    if (!ok)
        std::printf("FAIL n=%zu offset=%zu enabled=%zu stop=%d ready=0x%04x claim=%d: "
                    "expected %d%s, got %u (rounds %d, activate() calls %zu, disable() calls %zu)\n",
                    c.n, c.offset, c.enabled, (int)c.stop, (unsigned)c.ready, c.claim, expected,
                    round1 < 0 ? " after a new round" : "", got, g_round, g_run.activates,
                    g_run.disabled.size());
    return ok;
}

// Expected result from the 3.0.0 loop (claimed: the claimed guard, unchanged rule).
static bool checkAgainstOld(const Case& c, Counts& counts) {
    size_t order[ALT_MAX_GUARDS];
    bool   ready[ALT_MAX_GUARDS];
    for (size_t k = 0; k < c.enabled; ++k) {
        order[k] = (c.offset + k) % c.n;
        ready[k] = (c.ready >> k) & 1U;
    }
    int round1 = c.claim >= 0 ? (int)order[c.claim] : oldSelect(order, c.enabled, ready);
    int expected = round1 >= 0 ? round1 : (int)c.offset;      // round 2: order[0]
    return check(c, round1, expected, counts);
}

// Hand-checked cases: {n, offset, enabled, stop, ready (bit k = position k), claim}, winner.
struct Row { Case c; int winner; const char* what; };
static const Row kTable[] = {
    {{ 1,  0,  1, true,  0x0001, -1},  0, "one guard, ready"},
    {{ 1,  0,  1, false, 0x0000, -1},  0, "one guard, stale wakeup: new round, guard 0"},
    {{ 2,  1,  2, false, 0x0003, -1},  1, "both ready, offset 1: guard 1"},
    {{ 4,  2,  4, false, 0x000A, -1},  3, "positions 1 and 3 ready, order 2,3,0,1: guard 3"},
    {{ 4,  2,  4, false, 0x0008, -1},  1, "only position 3 ready, order 2,3,0,1: guard 1"},
    {{ 5,  3,  2, true,  0x0002, -1},  4, "phase 1 stopped at guard 4 (position 1), re-verified"},
    {{ 5,  3,  2, true,  0x0001, -1},  3, "stopped at guard 4, but only guard 3 re-verified"},
    {{ 5,  3,  2, true,  0x0000, -1},  3, "stopped at guard 4, withdrawn: new round, guard 3"},
    {{16,  0, 16, false, 0xFFFF, -1},  0, "16 guards, all ready, offset 0: guard 0"},
    {{16,  7, 16, false, 0xFFFF, -1},  7, "16 guards, all ready, offset 7: guard 7"},
    {{16, 15, 16, false, 0x8000, -1}, 14, "16 guards, only position 15 ready, offset 15: guard 14"},
    {{16,  9, 16, false, 0x0000, -1},  9, "16 guards, none ready: new round, guard 9"},
    {{16,  1, 16, false, 0x0410, -1},  5, "positions 4 and 10 ready, offset 1: guard 5"},
    {{ 3,  1,  2, true,  0x0001,  1},  2, "claimed for guard 2 (position 1): wins over ready guard 1"},
    {{ 3,  1,  1, true,  0x0000,  0},  1, "claimed for guard 1, not re-verified: the claim wins"},
};

int main() {
    bool ok = true;

    Counts table;
    for (const Row& r : kTable) {
        // The stated winner, and the 3.0.0 loop's.
        int round1 = r.winner;
        if (r.c.claim < 0 && r.c.ready == 0U) round1 = -1;
        bool row_ok = check(r.c, round1, r.winner, table);
        row_ok &= checkAgainstOld(r.c, table);
        if (!row_ok) std::printf("FAIL table row: %s\n", r.what);
        ok &= row_ok;
    }
    std::printf("table: %zu rows, %s\n", sizeof kTable / sizeof kTable[0], ok ? "all pass" : "FAIL");

    Counts all;
    for (size_t n = 1; n <= ALT_MAX_GUARDS; ++n)
        for (size_t offset = 0; offset < n; ++offset)
            for (size_t enabled = 1; enabled <= n; ++enabled)
                // stop: phase 1 stopped at position enabled - 1; with all n
                // enabled, also the round that enabled all and waited.
                for (int stop = (enabled == n ? 0 : 1); stop <= 1; ++stop) {
                    for (uint32_t ready = 0; ready < (1UL << enabled); ++ready)
                        ok &= checkAgainstOld({n, offset, enabled, stop != 0, ready, -1}, all);
                    if (stop) {                    // claimed at the stopping guard
                        const uint32_t masks[] = {0U, (1U << enabled) - 1U, 1U};
                        for (uint32_t m : masks)
                            ok &= checkAgainstOld({n, offset, enabled, true, m, (int)enabled - 1}, all);
                    }
                }
    std::printf("exhaustive: %lu cases (no guard ready %lu, one %lu, several %lu, claimed %lu)\n",
                all.cases, all.none, all.one, all.several, all.claimed);
    std::printf("%s\n", ok ? "PASS: select() picks the 3.0.0 loop's winner in every case" : "FAIL");
    return ok ? 0 : 1;
}
