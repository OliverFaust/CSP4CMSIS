#include "csp/sync_channel.h"
#include <cstring>

namespace csp::internal {

// Interval to check for task suspension or timeouts during blocking.
// osKernelGetTickFreq() (runtime) replaces FreeRTOS's pdMS_TO_TICKS()
// (a compile-time macro built on configTICK_RATE_HZ) -- see time.h's
// same substitution.
static uint32_t wait_slice_ticks() {
    return (100U * osKernelGetTickFreq()) / 1000U;
}

// =============================================================
// SyncChannel Core Implementation
// =============================================================

template <csp::BufferPolicy P>
SyncChannel<P>::SyncChannel()
    : state(IDLE),
      data_ptr(nullptr),
      data_len(0),
      waiting_alt_in(nullptr),
      waiting_alt_bit_in(0),
      waiting_guard_in(nullptr),
      waiting_alt_out(nullptr),
      waiting_alt_bit_out(0),
      waiting_guard_out(nullptr),
      res_in_guard(this),
      res_out_guard(this)
{
    mutex = osMutexNew(NULL);
    // Binary semaphores (max count 1, initial count 0 -- starts
    // "unavailable") replace the original's zero-item-size queues used
    // purely as rendezvous pulses. See sync_channel.h's own comment on
    // this substitution.
    sender_sem   = osSemaphoreNew(1, 0, NULL);
    receiver_sem = osSemaphoreNew(1, 0, NULL);
}

template <csp::BufferPolicy P>
SyncChannel<P>::~SyncChannel() {
    if (mutex) osMutexDelete(mutex);
    if (sender_sem) osSemaphoreDelete(sender_sem);
    if (receiver_sem) osSemaphoreDelete(receiver_sem);
}

template <csp::BufferPolicy P>
void SyncChannel<P>::reset() {
    state = IDLE;
    data_ptr = nullptr;
    waiting_alt_in = nullptr;
    waiting_alt_bit_in = 0;
    waiting_guard_in = nullptr;
    waiting_alt_out = nullptr;
    waiting_alt_bit_out = 0;
    waiting_guard_out = nullptr;
}

// --- Blocking/Sampling Output (Sender) ---
template <csp::BufferPolicy P>
void SyncChannel<P>::output(const void* const data_ptr_in) {
    if (osMutexAcquire(mutex, osWaitForever) != osOK) return;

    if (state == RECEIVER_WAITING) {
        data_ptr = data_ptr_in;

        AltScheduler* rx_alt = waiting_alt_in;
        uint32_t rx_alt_bit = waiting_alt_bit_in;
        bool is_alt_waiter = (waiting_alt_in != nullptr);

        waiting_alt_in = nullptr;
        osMutexRelease(mutex);

        if (is_alt_waiter) {
            rx_alt->wakeUp(rx_alt_bit);
        } else {
            osSemaphoreRelease(receiver_sem);
        }

        // BLOCK: Wait for receiver to finish copying
        while (osSemaphoreAcquire(sender_sem, wait_slice_ticks()) != osOK);
    }
    else {
        if constexpr (P != csp::BufferPolicy::Block) {
            osMutexRelease(mutex);
            return;
        } else {
            state = SENDER_WAITING;
            data_ptr = data_ptr_in;
            osMutexRelease(mutex);

            while (osSemaphoreAcquire(sender_sem, wait_slice_ticks()) != osOK);
        }
    }
}

// --- Blocking Input (Receiver) ---
template <csp::BufferPolicy P>
void SyncChannel<P>::input(void* const data_ptr_out) {
    if (osMutexAcquire(mutex, osWaitForever) != osOK) return;

    if (state == SENDER_WAITING) {
        if (data_ptr != nullptr && data_ptr_out != nullptr) {
            memcpy(data_ptr_out, data_ptr, data_len);
        }

        AltScheduler* tx_alt = waiting_alt_out;
        bool is_alt_waiter = (waiting_alt_out != nullptr);
        uint32_t tx_alt_bit = waiting_alt_bit_out;

        waiting_alt_out = nullptr;

        if (is_alt_waiter) {
            tx_alt->wakeUp(tx_alt_bit);
        } else {
            osSemaphoreRelease(sender_sem);
        }

        this->reset();
        osMutexRelease(mutex);
    }
    else {
        state = RECEIVER_WAITING;
        osMutexRelease(mutex);

        while (osSemaphoreAcquire(receiver_sem, wait_slice_ticks()) != osOK);

        osMutexAcquire(mutex, osWaitForever);
        if (data_ptr != nullptr && data_ptr_out != nullptr) {
            memcpy(data_ptr_out, data_ptr, data_len);
        }
        osSemaphoreRelease(sender_sem);
        this->reset();
        osMutexRelease(mutex);
    }
}

template <csp::BufferPolicy P>
bool SyncChannel<P>::pending() {
    return (state != IDLE);
}

template <csp::BufferPolicy P>
bool SyncChannel<P>::space_available() {
    if constexpr (P != csp::BufferPolicy::Block) return true;
    return (state == RECEIVER_WAITING || waiting_alt_in != nullptr);
}

template <csp::BufferPolicy P>
bool SyncChannel<P>::putFromISR() {
    bool success = false;

    if (state == RECEIVER_WAITING) {
        // Since we can't notify via receiver_sem comfortably here,
        // Sync Signals from ISR are usually simplified or handled via Buffered channels.
        success = true;
    } else if (waiting_alt_in != nullptr) {
        waiting_alt_in->wakeUp(waiting_alt_bit_in);
        success = true;
    } else if constexpr (P != csp::BufferPolicy::Block) {
        success = true;
    }

    // No explicit yield-from-ISR call needed -- osEventFlagsSet()
    // (inside wakeUp(), called above when applicable) detects IRQ
    // context internally, unlike FreeRTOS's xEventGroupSetBitsFromISR()
    // + portYIELD_FROM_ISR() pair.
    return success;
}

// =============================================================
// ALT Registration Logic
// =============================================================

template <csp::BufferPolicy P>
bool SyncChannel<P>::registerAltIn(AltScheduler* alt, uint32_t bit, SyncChannelInputGuard<P>* guard) {
    if (osMutexAcquire(mutex, osWaitForever) != osOK) return false;

    if (state == SENDER_WAITING) {
        osMutexRelease(mutex);
        return true;
    }

    state = RECEIVER_WAITING;
    waiting_alt_in = alt;
    waiting_alt_bit_in = bit;
    waiting_guard_in = guard;
    osMutexRelease(mutex);
    return false;
}

template <csp::BufferPolicy P>
bool SyncChannel<P>::unregisterAltIn(AltScheduler* alt) {
    if (osMutexAcquire(mutex, osWaitForever) != osOK) return false;
    bool completed = (state != RECEIVER_WAITING);
    // Passing nullptr from Guard disable() means we check by current state
    if (!completed && (alt == nullptr || waiting_alt_in == alt)) this->reset();
    osMutexRelease(mutex);
    return completed;
}

template <csp::BufferPolicy P>
bool SyncChannel<P>::registerAltOut(AltScheduler* alt, uint32_t bit, SyncChannelOutputGuard<P>* guard) {
    if (osMutexAcquire(mutex, osWaitForever) != osOK) return false;

    if constexpr (P != csp::BufferPolicy::Block) {
        osMutexRelease(mutex);
        return true;
    }

    if (state == RECEIVER_WAITING) {
        osMutexRelease(mutex);
        return true;
    }

    state = SENDER_WAITING;
    waiting_alt_out = alt;
    waiting_alt_bit_out = bit;
    waiting_guard_out = guard;
    osMutexRelease(mutex);
    return false;
}

template <csp::BufferPolicy P>
bool SyncChannel<P>::unregisterAltOut(AltScheduler* alt) {
    if (osMutexAcquire(mutex, osWaitForever) != osOK) return false;
    bool completed = (state != SENDER_WAITING);
    if (!completed && (alt == nullptr || waiting_alt_out == alt)) this->reset();
    osMutexRelease(mutex);
    return completed;
}

// =============================================================
// Guard Implementation
// =============================================================

template <csp::BufferPolicy P>
bool SyncChannelInputGuard<P>::enable(AltScheduler* alt, uint32_t bit) {
    return channel->registerAltIn(alt, bit, this);
}

template <csp::BufferPolicy P>
bool SyncChannelInputGuard<P>::disable() {
    return channel->unregisterAltIn(nullptr);
}

template <csp::BufferPolicy P>
void SyncChannelInputGuard<P>::activate() {
    osMutexAcquire(channel->getMutex(), osWaitForever);
    if (channel->getDataPtr() != nullptr && user_data_dest != nullptr) {
        // data_size should be set during bind()
        memcpy(user_data_dest, channel->getDataPtr(), data_size);
    }
    osSemaphoreRelease(channel->getSenderSem());
    channel->reset();
    osMutexRelease(channel->getMutex());
}

template <csp::BufferPolicy P>
bool SyncChannelOutputGuard<P>::enable(AltScheduler* alt, uint32_t bit) {
    return channel->registerAltOut(alt, bit, this);
}

template <csp::BufferPolicy P>
bool SyncChannelOutputGuard<P>::disable() {
    return channel->unregisterAltOut(nullptr);
}

template <csp::BufferPolicy P>
void SyncChannelOutputGuard<P>::activate() {
    channel->output(user_data_source);
}

// =============================================================
// Explicit Instantiations
// =============================================================

template class SyncChannel<csp::BufferPolicy::Block>;
template class SyncChannel<csp::BufferPolicy::KeepNewest>;
template class SyncChannel<csp::BufferPolicy::KeepOldest>;

template class SyncChannelInputGuard<csp::BufferPolicy::Block>;
template class SyncChannelInputGuard<csp::BufferPolicy::KeepNewest>;
template class SyncChannelInputGuard<csp::BufferPolicy::KeepOldest>;

template class SyncChannelOutputGuard<csp::BufferPolicy::Block>;
template class SyncChannelOutputGuard<csp::BufferPolicy::KeepNewest>;
template class SyncChannelOutputGuard<csp::BufferPolicy::KeepOldest>;

} // namespace csp::internal
