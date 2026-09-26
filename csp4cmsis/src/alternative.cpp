#include "csp/alt.h"
#include <cstdio>
// CMSIS compiler intrinsics (__CLZ, etc.) -- typically pulled in via the
// device header, but included explicitly here since this file depends on
// __CLZ directly.
extern "C" {
    #include <cmsis_compiler.h>
}

namespace csp::internal {

// =============================================================
// AltScheduler Implementation
// =============================================================

unsigned int AltScheduler::select(Guard** guardArray, size_t amount, size_t offset) {
    if (amount == 0) return 0;
    if (amount > ALT_MAX_GUARDS) fatal("CSP4CMSIS: Alternative: more than 16 guards");

    owner = osThreadGetId();

    uint32_t wait_mask = 0;
    for (size_t i = 0; i < amount; ++i) wait_mask |= altFlag(i);

    size_t order[ALT_MAX_GUARDS];

    for (;;) {
        // Drop any wakeup left over from an earlier round or select().
        (void)osThreadFlagsClear(wait_mask);

        // Phase 1: Enable, starting at 'offset' (fair ALT). Stops at the
        // first guard that is ready now.
        size_t enabled = 0;
        int ready_idx = -1;
        for (size_t i = 0; i < amount; ++i) {
            size_t idx = (i + offset) % amount;
            order[enabled++] = idx;
            if (guardArray[idx]->enable(this, altFlag(idx))) {
                ready_idx = (int)idx;
                break;
            }
        }

        // Phase 2: Wait (only if nothing was ready).
        size_t selected;
        if (ready_idx >= 0) {
            selected = (size_t)ready_idx;
        } else {
            uint32_t r = osThreadFlagsWait(wait_mask, osFlagsWaitAny, osWaitForever);
            if ((r & osFlagsError) != 0U) {
                for (size_t k = 0; k < enabled; ++k) (void)guardArray[order[k]]->disable();
                fatal("CSP4CMSIS: Alternative: osThreadFlagsWait() failed");
            }
            uint32_t fired = (r & wait_mask) >> ALT_FLAG_SHIFT;
            if (fired == 0U) {                       // not ours: disable, retry
                for (size_t k = 0; k < enabled; ++k) (void)guardArray[order[k]]->disable();
                continue;
            }
            // Priority follows guard order starting at 'offset' (fair ALT):
            // look at indices >= offset first, then wrap around. Within the
            // chosen half the lowest set bit is isolated (x & -x) and __CLZ
            // gives its index in O(1).
            uint32_t offset_mask  = 0xFFFFFFFFU << offset;
            uint32_t masked_fired = fired & offset_mask;
            uint32_t candidate    = (masked_fired != 0U) ? masked_fired : fired;
            uint32_t lowest_bit   = candidate & (uint32_t)(-(int32_t)candidate);
            selected = 31U - __CLZ(lowest_bit);
        }

        // Phase 3: Disable exactly the guards enabled in this round; they
        // report whether they can complete now.
        bool selected_ready = false;
        for (size_t k = 0; k < enabled; ++k) {
            bool ready = guardArray[order[k]]->disable();
            if (order[k] == selected) selected_ready = ready;
        }

        // Phase 4: Re-verify (stale wakeups), then commit.
        if (!guardArray[selected]->confirm(selected_ready)) continue;
        if (!guardArray[selected]->activate()) continue;
        return (unsigned int)selected;
    }
}

void AltScheduler::wakeUp(uint32_t flag) {
    osThreadId_t t = owner;
    if (t != nullptr) (void)osThreadFlagsSet(t, flag);
}

// =============================================================
// TimerGuard Implementation
// =============================================================
TimerGuard::TimerGuard(csp::Time delay)
    : delay_ticks(delay.to_ticks()), timer_handle(nullptr),
      wake_thread(nullptr), wake_flag(0), fired(false)
{
    osTimerAttr_t attr = {};
    attr.name = "CspTimeout";
#if defined(CSP4CMSIS_STATIC_ALLOCATION)
    attr.cb_mem  = &timer_storage;
    attr.cb_size = sizeof(timer_storage);
#endif
    timer_handle = osTimerNew(TimerCallback, osTimerOnce, this, &attr);
    if (timer_handle == nullptr) fatal("CSP4CMSIS: TimerGuard: osTimerNew() failed");
}

TimerGuard::~TimerGuard() {
    if (timer_handle) osTimerDelete(timer_handle);
}

void TimerGuard::TimerCallback(void* argument) {
    // Runs in the RTOS timer thread. Works from the snapshot taken in
    // enable(); a callback that races with disable() only leaves a stale
    // flag, which select() clears/re-verifies.
    auto* s = static_cast<TimerGuard*>(argument);
    s->fired = true;
    osThreadId_t t = s->wake_thread;
    if (t != nullptr) (void)osThreadFlagsSet(t, s->wake_flag);
}

bool TimerGuard::enable(AltScheduler* a, uint32_t flag) {
    fired = false;
    wake_flag = flag;
    wake_thread = a->ownerThread();
    if (delay_ticks == 0) { fired = true; return true; }   // instant timeout
    (void)osTimerStart(timer_handle, delay_ticks);
    return false;
}

bool TimerGuard::disable() {
    (void)osTimerStop(timer_handle);
    wake_thread = nullptr;
    return fired;
}

} // namespace csp::internal

namespace csp {

// =============================================================
// Alternative Implementation
// =============================================================

Alternative::Alternative(std::initializer_list<internal::Guard*> list) {
    num_guards = 0;
    for (auto* g : list) {
        if (num_guards < MAX_GUARDS) internal_guards[num_guards++] = g;
    }
}

Alternative::Alternative(std::initializer_list<Guard*> list) {
    num_guards = 0;
    for (auto* g : list) {
        if (num_guards < MAX_GUARDS) internal_guards[num_guards++] = g->internal_guard_ptr;
    }
}

int Alternative::priSelect() {
    return (int)internal_alt.select(internal_guards, num_guards, 0);
}

int Alternative::fairSelect() {
    if (num_guards <= 1) return priSelect();

    size_t actual_index = internal_alt.select(internal_guards, num_guards, fair_select_start_index);

    // Update fairness index to ensure next guard has priority next time
    fair_select_start_index = (actual_index + 1) % num_guards;

    return (int)actual_index;
}

} // namespace csp
