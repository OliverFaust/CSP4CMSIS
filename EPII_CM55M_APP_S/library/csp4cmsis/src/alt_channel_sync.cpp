#include "alt_channel_sync.h"
#include "alt.h"
#include <cstring>
#include <cstdio>

namespace csp::internal {

// =============================================================
// AltChanSyncBase Implementation
// =============================================================

AltChanSyncBase::AltChanSyncBase() {
    // In a fully static system, xSemaphoreCreateMutexStatic could be used here
    mutex = xSemaphoreCreateMutex();
    
    // Initialize POD structures
    waiting_in_alt = {nullptr, 0, nullptr, nullptr, 0};
    waiting_out_alt = {nullptr, 0, nullptr, nullptr, 0};
    
    waiting_in_task = nullptr;
    waiting_out_task = nullptr;
    non_alt_in_data_ptr = nullptr;
    non_alt_out_data_ptr = nullptr;
    
    if (mutex == NULL) {
        // Use a standard hook or printf for startup errors
        printf("CRITICAL: CSP Mutex Allocation Failed\r\n");
    }
}

AltChanSyncBase::~AltChanSyncBase() {
    if (mutex) vSemaphoreDelete(mutex);
}

void AltChanSyncBase::clearWaitingOut() {
    waiting_out_task = nullptr;
    non_alt_out_data_ptr = nullptr;
}

void AltChanSyncBase::clearWaitingIn() {
    waiting_in_task = nullptr;
    non_alt_in_data_ptr = nullptr;
}

Guard* AltChanSyncBase::tryRendezvous(void* data_ptr, size_t size, bool is_writer) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdPASS) return nullptr;

    if (is_writer) {
        // I am a Sender. Is there an ALT Receiver already waiting?
        if (waiting_in_alt.alt_ptr != nullptr) {
            AltScheduler* sched = waiting_in_alt.alt_ptr;
            EventBits_t bit = waiting_in_alt.assigned_bit;
            
            // Register this sender's data for the Receiver's activate() to pull
            waiting_out_task = xTaskGetCurrentTaskHandle();
            non_alt_out_data_ptr = data_ptr;

            xSemaphoreGive(mutex);
            sched->wakeUp(bit);
            return reinterpret_cast<Guard*>(0x1); 
        }
        // Is there a standard blocking Receiver?
        if (waiting_in_task != nullptr) {
            if (non_alt_in_data_ptr && data_ptr) {
                memcpy(non_alt_in_data_ptr, data_ptr, size);
            }
            TaskHandle_t t = waiting_in_task;
            clearWaitingIn();
            xSemaphoreGive(mutex);
            xTaskNotifyGive(t);
            return reinterpret_cast<Guard*>(0x1); 
        }
    } else {
        // I am a Receiver. Is there an ALT Sender already waiting?
        if (waiting_out_alt.alt_ptr != nullptr) {
            AltScheduler* sched = waiting_out_alt.alt_ptr;
            EventBits_t bit = waiting_out_alt.assigned_bit;

            // Register this receiver's dest for the Sender's activate() to push to
            waiting_in_task = xTaskGetCurrentTaskHandle();
            non_alt_in_data_ptr = data_ptr;

            xSemaphoreGive(mutex);
            sched->wakeUp(bit);
            return reinterpret_cast<Guard*>(0x1);
        }
        // Is there a standard blocking Sender?
        if (waiting_out_task != nullptr) {
            if (data_ptr && non_alt_out_data_ptr) {
                memcpy(data_ptr, non_alt_out_data_ptr, size);
            }
            TaskHandle_t t = waiting_out_task;
            clearWaitingOut();
            xSemaphoreGive(mutex);
            xTaskNotifyGive(t);
            return reinterpret_cast<Guard*>(0x1);
        }
    }

    xSemaphoreGive(mutex);
    return nullptr;
}

bool AltChanSyncBase::registerBlockingTask(void* data_ptr, bool is_writer) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    if (is_writer) {
        waiting_out_task = xTaskGetCurrentTaskHandle();
        non_alt_out_data_ptr = data_ptr;
    } else {
        waiting_in_task = xTaskGetCurrentTaskHandle();
        non_alt_in_data_ptr = data_ptr;
    }
    xSemaphoreGive(mutex);
    return true;
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