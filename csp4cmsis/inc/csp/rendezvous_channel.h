#ifndef CSP4CMSIS_RENDEZVOUS_CHANNEL_H
#define CSP4CMSIS_RENDEZVOUS_CHANNEL_H

#include "channel_base.h"
#include "alt_channel_sync.h"
#include "cmsis_os2.h"
#include "csp_critical.h"
#include <cstring>

namespace csp::internal {

/**
 * @brief Zero-capacity Synchronous Channel (Rendezvous).
 * Updated to support KeepNewest/KeepOldest (Non-blocking handshake).
 */
template <typename T, csp::BufferPolicy P = csp::BufferPolicy::Block>
class RendezvousChannel : public BaseAltChan<T> {
private:
    AltChanSyncBase sync_base;

public:
    RendezvousChannel() = default;

    virtual ~RendezvousChannel() override = default;

    // --- Core Contract Overrides ---

    /**
     * @brief In Rendezvous, space is available only if a receiver is waiting.
     * UNLESS the policy is non-blocking, in which case we are "always ready"
     * because we'll just drop the data if no one is there.
     */
    bool space_available() override {
        if constexpr (P != csp::BufferPolicy::Block) return true;

        bool ready = false;
        if (osMutexAcquire(sync_base.getMutex(), 0) == osOK) {
            ready = (sync_base.getWaitingInTask() != nullptr) ||
                    (sync_base.getAltInScheduler() != nullptr);
            osMutexRelease(sync_base.getMutex());
        }
        return ready;
    }

    bool pending() override {
        bool has_partner = false;
        if (osMutexAcquire(sync_base.getMutex(), 0) == osOK) {
            has_partner = (sync_base.getWaitingInTask() != nullptr) ||
                          (sync_base.getWaitingOutTask() != nullptr) ||
                          (sync_base.getAltInScheduler() != nullptr) ||
                          (sync_base.getAltOutScheduler() != nullptr);
            osMutexRelease(sync_base.getMutex());
        }
        return has_partner;
    }

    // --- Blocking Input (Receiver) ---
    // Receiver always blocks in Rendezvous, regardless of policy.
    void input(T* const dest) override {
        // Defensive clear before registering as a waiter, mirroring the
        // original's xTaskNotifyStateClear(NULL) -- ensures a stray
        // pre-existing flag isn't mistaken for a fresh handshake below.
        osThreadFlagsClear(RENDEZVOUS_FLAG);

        if (osMutexAcquire(sync_base.getMutex(), osWaitForever) == osOK) {
            if (sync_base.tryHandshake((void*)dest, sizeof(T), false)) {
                osMutexRelease(sync_base.getMutex());
                return;
            }

            if (sync_base.getAltOutScheduler() != nullptr) {
                sync_base.getAltOutScheduler()->wakeUp(sync_base.getAltOutBit());
            }

            sync_base.registerWaitingTask((void*)dest, false);
            osMutexRelease(sync_base.getMutex());
        }
        osThreadFlagsWait(RENDEZVOUS_FLAG, osFlagsWaitAny, osWaitForever);
    }

    // --- Policy-Aware Output (Sender) ---
    void output(const T* const source) override {
        osThreadFlagsClear(RENDEZVOUS_FLAG);

        if (osMutexAcquire(sync_base.getMutex(), osWaitForever) == osOK) {
            // 1. Try immediate handshake (Standard or ALT reader)
            if (sync_base.getWaitingInTask() != nullptr) {
                sync_base.tryHandshake((void*)const_cast<T*>(source), sizeof(T), true);
                osMutexRelease(sync_base.getMutex());
                return;
            }

            if (sync_base.getAltInScheduler() != nullptr) {
                sync_base.getAltInScheduler()->wakeUp(sync_base.getAltInBit());
                // In Rendezvous, we still block until they 'activate' the ALT
            }

            // 2. Policy Check: If we reach here, no receiver was immediately ready.
            if constexpr (P != csp::BufferPolicy::Block) {
                // Non-blocking policy: Drop the data and exit.
                osMutexRelease(sync_base.getMutex());
                return;
            }

            // 3. Blocking Path: Register and wait
            sync_base.registerWaitingTask((void*)const_cast<T*>(source), true);
            osMutexRelease(sync_base.getMutex());
        }
        osThreadFlagsWait(RENDEZVOUS_FLAG, osFlagsWaitAny, osWaitForever);
    }

    // --- ISR Output ---
    bool putFromISR(const T& data) override {
        bool success = false;
        osThreadId_t  wake_task = nullptr;
        AltScheduler* wake_alt  = nullptr;
        uint32_t      wake_flag = 0;

        // State is inspected and updated inside the CSP critical section;
        // the RTOS is only called after leaving it (no RTOS call may run
        // with BASEPRI raised -- see csp_critical.h).
        uint32_t saved = csp_enter_critical();
        if (sync_base.getWaitingInTask() != nullptr) {
            std::memcpy(sync_base.getNonAltInDataPtr(), &data, sizeof(T));
            wake_task = sync_base.getWaitingInTask();
            sync_base.clearWaitingIn();
            success = true;
        }
        else if (sync_base.getAltInScheduler() != nullptr) {
            wake_alt  = sync_base.getAltInScheduler();
            wake_flag = sync_base.getAltInBit();
            success = true;
        }
        else if constexpr (P != csp::BufferPolicy::Block) {
            // Non-blocking ISR: Treat "dropped" as "handled successfully"
            success = true;
        }
        csp_exit_critical(saved);

        // osThreadFlagsSet() detects IRQ context internally (no separate
        // yield-from-ISR call needed).
        if (wake_task != nullptr) osThreadFlagsSet(wake_task, RENDEZVOUS_FLAG);
        if (wake_alt  != nullptr) wake_alt->wakeUp(wake_flag);
        return success;
    }

    virtual internal::Guard* getInputGuard(GuardSlot& slot, T& dest) override {
        return slot.emplace<ChanInGuard>(&sync_base, static_cast<void*>(&dest), sizeof(T));
    }

    virtual internal::Guard* getOutputGuard(GuardSlot& slot, const T& source) override {
        return slot.emplace<ChanOutGuard>(&sync_base, static_cast<const void*>(&source), sizeof(T));
    }

    void beginExtInput(T* const /*dest*/) override {}
    void endExtInput() override {}
};

} // namespace csp::internal

#endif
