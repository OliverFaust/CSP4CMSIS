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
AltScheduler::AltScheduler() {
    initForCurrentTask();
}

AltScheduler::~AltScheduler() {
    if (event_group) osEventFlagsDelete(event_group);
}

void AltScheduler::initForCurrentTask() {
    waiting_task_handle = osThreadGetId();

    osEventFlagsAttr_t attr = {};
    attr.name = "AltEventFlags";
#if defined(CSP4CMSIS_STATIC_ALLOCATION)
    // Static allocation: event_group_buffer is a csp_static_eventflags_
    // storage_t member of AltScheduler (see alt.h, csp_rtos_static.h).
    // This keeps AltScheduler -- and therefore every Alternative, since
    // it owns one AltScheduler -- free of heap allocation. Each backend's
    // own static osEventFlagsNew() path recognizes a caller-supplied
    // control block via cb_mem/cb_size (confirmed directly against each
    // backend's source in csp_rtos_static.h, not assumed).
    attr.cb_mem  = &event_group_buffer;
    attr.cb_size = sizeof(event_group_buffer);
#endif
    // When CSP4CMSIS_STATIC_ALLOCATION isn't defined, cb_mem/cb_size stay
    // at their zero-initialized default (NULL/0) -- every CMSIS-RTOS2
    // backend supports this as its dynamic (heap-backed) path.
    event_group = osEventFlagsNew(&attr);
}

unsigned int AltScheduler::select(Guard** guardArray, size_t amount, size_t offset) {
    if (amount == 0) return 0;

    uint32_t wait_mask = 0;
    for(size_t i = 0; i < amount; ++i) wait_mask |= (1 << i);

    osEventFlagsClear(event_group, wait_mask);

    int ready_idx = -1;

    // Phase 1: Enable
    // We poll guards starting from 'offset' to support Fair ALT
    for(size_t i = 0; i < amount; ++i) {
        size_t idx = (i + offset) % amount;

        // If enable() returns true, that guard is ready IMMEDIATELY
        if(guardArray[idx]->enable(this, (1 << idx))) {
            ready_idx = (int)idx;
            break;
        }
    }

    // Phase 2: Wait
    uint32_t fired = 0;
    if (ready_idx != -1) {
        // We already found a winner in the Enable phase
        fired = (1 << ready_idx);
    } else {
        // Block until a channel becomes ready or a timer expires.
        // osFlagsWaitAny + default clears matched bits on return, matching
        // the original xEventGroupWaitBits(..., pdTRUE, pdFALSE, ...)'s
        // "clear on exit" behavior.
        fired = osEventFlagsWait(event_group, wait_mask, osFlagsWaitAny, osWaitForever);
    }

    // Phase 2.5: Identify which guard fired.
    //
    // Priority follows bit order (bit 0 = highest), but fairSelect()
    // rotates the starting priority via 'offset' each call to avoid
    // starvation. We therefore look first at bits >= offset, and only
    // wrap around to bits < offset if nothing fired there. Within
    // whichever half is chosen, the lowest set bit is isolated with the
    // standard two's-complement trick (x & -x), and __CLZ resolves its
    // index in O(1) -- a single cycle on any Cortex-M core with a
    // hardware CLZ instruction (Cortex-M3/M4/M7 and Armv8-M Mainline),
    // and a short bounded software emulation on cores without one
    // (Cortex-M0/M0+/M1/M23), since __CLZ is a CMSIS intrinsic rather
    // than raw inline assembly.
    size_t selected = 0;
    if (fired != 0) {
        uint32_t offset_mask = 0xFFFFFFFFU << offset;
        uint32_t masked_fired = fired & offset_mask;

        uint32_t candidate = (masked_fired != 0) ? masked_fired : fired;
        uint32_t lowest_set_bit = candidate & (uint32_t)(-(int32_t)candidate);
        selected = 31 - __CLZ(lowest_set_bit);
    }

    // Phase 3: Disable
    // Crucial: We tell guards we are leaving. If disable() returns true for a
    // guard we didn't 'select', it means a rendezvous almost happened but we
    // missed it—this is handled by the internal channel state machines.
    for(size_t i = 0; i < amount; ++i) {
        guardArray[i]->disable();
    }

    // Phase 4: Activate
    // The "Commit" phase. For Rendezvous, this moves the data.
    // For sampling channels, this might result in a 'no-op' if data was dropped.
    guardArray[selected]->activate();

    return (unsigned int)selected;
}

void AltScheduler::wakeUp(uint32_t bit) {
    if(!event_group) return;

    // CMSIS-RTOS2 detects IRQ context internally -- osEventFlagsSet() is
    // safe to call from either context, unlike FreeRTOS's split
    // xEventGroupSetBits()/xEventGroupSetBitsFromISR()+portYIELD_FROM_ISR
    // pair. One call replaces both.
    osEventFlagsSet(event_group, bit);
}

// =============================================================
// TimerGuard Implementation
// =============================================================
TimerGuard::TimerGuard(csp::Time delay)
    : parent_alt(nullptr), delay_ticks(delay.to_ticks()), assigned_bit(0)
{
    timer_handle = osTimerNew(TimerCallback, osTimerOnce, this, NULL);
}

TimerGuard::~TimerGuard() {
    if (timer_handle) osTimerDelete(timer_handle);
}

void TimerGuard::TimerCallback(void* argument) {
    // CMSIS-RTOS2 passes the argument given to osTimerNew() straight to
    // the callback -- no TimerHandle_t-to-owner lookup needed (FreeRTOS's
    // pvTimerGetTimerID(x) is gone entirely, not just renamed).
    auto* s = static_cast<TimerGuard*>(argument);
    if(s && s->parent_alt) {
        s->parent_alt->wakeUp(s->assigned_bit);
    }
}

bool TimerGuard::enable(AltScheduler* a, uint32_t b) {
    parent_alt = a;
    assigned_bit = b;
    if (delay_ticks == 0) return true; // Instant timeout
    osTimerStart(timer_handle, delay_ticks);
    return false;
}

bool TimerGuard::disable() {
    if (timer_handle) osTimerStop(timer_handle);
    return true;
}

void TimerGuard::activate() {}

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
