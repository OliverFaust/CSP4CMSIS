#include "csp/alt_channel_sync.h"
#include "csp/alt.h"
#include <cstring>

namespace csp::internal {

// --- REMOVED: Constructor, Destructor, and Clear methods (moved to .h) ---

bool AltChanSyncBase::tryHandshake(void* data_ptr, size_t size, bool is_writer) {
    if (is_writer) {
        // Is a standard blocking Receiver already waiting?
        if (waiting_in_task != nullptr) {
            if (non_alt_in_data_ptr && data_ptr) {
                memcpy(non_alt_in_data_ptr, data_ptr, size);
            }
            TaskHandle_t t = waiting_in_task;
            clearWaitingIn();
            xTaskNotifyGive(t);
            return true; 
        }
        // Logic for ALT-ing Receiver omitted for brevity, add back if using ALT
    } else {
        // Is a standard blocking Sender already waiting?
        if (waiting_out_task != nullptr) {
            if (data_ptr && non_alt_out_data_ptr) {
                memcpy(data_ptr, non_alt_out_data_ptr, size);
            }
            TaskHandle_t t = waiting_out_task;
            clearWaitingOut();
            xTaskNotifyGive(t);
            return true;
        }
    }
    return false;
}

void AltChanSyncBase::registerWaitingTask(void* data_ptr, bool is_writer) {
    if (is_writer) {
        waiting_out_task = xTaskGetCurrentTaskHandle();
        non_alt_out_data_ptr = data_ptr;
    } else {
        waiting_in_task = xTaskGetCurrentTaskHandle();
        non_alt_in_data_ptr = data_ptr;
    }
}

// =============================================================
// ChanInGuard Implementation (Resident Guard)
// =============================================================

bool ChanInGuard::enable(AltScheduler* alt, EventBits_t bit) {
    if (xSemaphoreTake(parent_channel->getMutex(), portMAX_DELAY) != pdTRUE) return false;

    // Check if a Sender is already waiting to give us data
    if (parent_channel->getWaitingOutTask() != nullptr) {
        xSemaphoreGive(parent_channel->getMutex());
        return true; 
    }

    // Otherwise, register this ALT in the channel's resident slot
    WaitingAlt& slot = parent_channel->getWaitingInAlt();
    slot.alt_ptr = alt;
    slot.assigned_bit = bit;
    slot.guard_ptr = this;
    slot.data_ptr = user_data_dest; // Late-bound pointer from updateBuffer()
    slot.data_size = data_size;

    xSemaphoreGive(parent_channel->getMutex());
    return false;
}

void ChanInGuard::activate() {
    if (xSemaphoreTake(parent_channel->getMutex(), portMAX_DELAY) != pdTRUE) return;

    TaskHandle_t sender = parent_channel->getWaitingOutTask();
    if (sender != nullptr) {
        // Complete the rendezvous transfer
        if (user_data_dest && parent_channel->getNonAltOutDataPtr()) {
            memcpy(user_data_dest, parent_channel->getNonAltOutDataPtr(), data_size);
        }
        parent_channel->clearWaitingOut();
        xSemaphoreGive(parent_channel->getMutex());
        xTaskNotifyGive(sender);
    } else {
        xSemaphoreGive(parent_channel->getMutex());
    }
}

bool ChanInGuard::disable() {
    if (xSemaphoreTake(parent_channel->getMutex(), portMAX_DELAY) != pdTRUE) return false;
    parent_channel->getWaitingInAlt().alt_ptr = nullptr;
    xSemaphoreGive(parent_channel->getMutex());
    return true;
}

// =============================================================
// ChanOutGuard Implementation (Resident Guard)
// =============================================================

bool ChanOutGuard::enable(AltScheduler* alt, EventBits_t bit) {
    if (xSemaphoreTake(parent_channel->getMutex(), portMAX_DELAY) != pdTRUE) return false;

    // Check if a Receiver is already waiting for data
    if (parent_channel->getWaitingInTask() != nullptr) {
        xSemaphoreGive(parent_channel->getMutex());
        return true;
    }

    // Register our intent to send in the resident slot
    WaitingAlt& slot = parent_channel->getWaitingOutAlt();
    slot.alt_ptr = alt;
    slot.assigned_bit = bit;
    slot.guard_ptr = this;
    slot.data_ptr = const_cast<void*>(user_data_source);
    slot.data_size = data_size;

    xSemaphoreGive(parent_channel->getMutex());
    return false;
}

void ChanOutGuard::activate() {
    if (xSemaphoreTake(parent_channel->getMutex(), portMAX_DELAY) != pdTRUE) return;

    TaskHandle_t receiver = parent_channel->getWaitingInTask();
    if (receiver != nullptr) {
        // Push the data to the waiting receiver's buffer
        if (parent_channel->getNonAltInDataPtr() && user_data_source) {
            memcpy(parent_channel->getNonAltInDataPtr(), user_data_source, data_size);
        }
        parent_channel->clearWaitingIn();
        xSemaphoreGive(parent_channel->getMutex());
        xTaskNotifyGive(receiver);
    } else {
        xSemaphoreGive(parent_channel->getMutex());
    }
}

bool ChanOutGuard::disable() {
    if (xSemaphoreTake(parent_channel->getMutex(), portMAX_DELAY) != pdTRUE) return false;
    parent_channel->getWaitingOutAlt().alt_ptr = nullptr;
    xSemaphoreGive(parent_channel->getMutex());
    return true;
}

} // namespace csp::internal
