#include "csp/alt_channel_sync.h"
#include <cstring>
#include <cstdio>

namespace csp::internal {

// RENDEZVOUS_FLAG is defined in alt_channel_sync.h, shared with
// rendezvous_channel.h's own direct input()/output() handshake.

AltChanSyncBase::AltChanSyncBase() :
    mutex(nullptr), waiting_in_task(nullptr), waiting_out_task(nullptr),
    non_alt_in_data_ptr(nullptr), non_alt_out_data_ptr(nullptr)
{
    mutex = osMutexNew(NULL);
}

AltChanSyncBase::~AltChanSyncBase() {
    if (mutex != nullptr) osMutexDelete(mutex);
}

bool AltChanSyncBase::tryHandshake(void* data_ptr, size_t size, bool is_writer) {
    if (is_writer) {
        // 1. Check for a standard blocking receiver
        if (waiting_in_task != nullptr) {
            if (non_alt_in_data_ptr && data_ptr) {
                memcpy(non_alt_in_data_ptr, data_ptr, size);
            }
            osThreadId_t t = waiting_in_task;
            clearWaitingIn();
            osThreadFlagsSet(t, RENDEZVOUS_FLAG);
            return true;
        }
        // 2. Check for a receiver waiting in an ALT
        if (waiting_in_alt.isActive()) {
            if (waiting_in_alt.data_ptr && data_ptr) {
                memcpy(waiting_in_alt.data_ptr, data_ptr, size);
            }
            // Logic for ALTs: The caller of tryHandshake (output) must wake the AltScheduler
            return true;
        }
    } else {
        // 1. Check for a standard blocking sender
        if (waiting_out_task != nullptr) {
            if (data_ptr && non_alt_out_data_ptr) {
                memcpy(data_ptr, non_alt_out_data_ptr, size);
            }
            osThreadId_t t = waiting_out_task;
            clearWaitingOut();
            osThreadFlagsSet(t, RENDEZVOUS_FLAG);
            return true;
        }
        // 2. Check for a sender waiting in an ALT
        if (waiting_out_alt.isActive()) {
            if (data_ptr && waiting_out_alt.data_ptr) {
                memcpy(data_ptr, waiting_out_alt.data_ptr, size);
            }
            return true;
        }
    }
    return false;
}

void AltChanSyncBase::registerWaitingTask(void* data_ptr, bool is_writer) {
    if (is_writer) {
        waiting_out_task = osThreadGetId();
        non_alt_out_data_ptr = data_ptr;
    } else {
        waiting_in_task = osThreadGetId();
        non_alt_in_data_ptr = data_ptr;
    }
}

// --- ChanInGuard Implementation ---

bool ChanInGuard::enable(AltScheduler* alt, uint32_t bit) {
    if (osMutexAcquire(parent_channel->getMutex(), osWaitForever) != osOK) return false;

    // Is there a sender ready right now? (Handshake possible)
    if (parent_channel->hasWriterWaiting()) {
        osMutexRelease(parent_channel->getMutex());
        return true;
    }

    // Otherwise, register for later
    parent_channel->getWaitingInAlt().set(alt, bit, user_data_dest, data_size);
    osMutexRelease(parent_channel->getMutex());
    return false;
}

void ChanInGuard::activate() {
    if (osMutexAcquire(parent_channel->getMutex(), osWaitForever) != osOK) return;

    // Case 1: partner is a plain blocking task.
    osThreadId_t sender = parent_channel->getWaitingOutTask();
    if (sender != nullptr) {
        if (user_data_dest && parent_channel->getNonAltOutDataPtr())
            memcpy(user_data_dest, parent_channel->getNonAltOutDataPtr(), data_size);

        parent_channel->clearWaitingOut();
        osMutexRelease(parent_channel->getMutex());
        osThreadFlagsSet(sender, RENDEZVOUS_FLAG);
        return;
    }

    // Case 2: partner is another ALT (ALT-vs-ALT rendezvous). Without this
    // branch no data is ever copied and the partner's AltScheduler is never
    // woken -- it blocks until its own timeout guard (if any) fires.
    WaitingAlt& out_alt = parent_channel->getWaitingOutAlt();
    if (out_alt.isActive()) {
        AltScheduler* partner_alt = out_alt.alt_ptr;
        uint32_t      partner_bit = out_alt.assigned_bit;

        if (user_data_dest && out_alt.data_ptr)
            memcpy(user_data_dest, out_alt.data_ptr, data_size);

        out_alt.clear();
        osMutexRelease(parent_channel->getMutex());
        if (partner_alt) partner_alt->wakeUp(partner_bit);
        return;
    }

    // Case 3: nothing to do (e.g. data was already moved by tryHandshake()).
    osMutexRelease(parent_channel->getMutex());
}

bool ChanInGuard::disable() {
    if (osMutexAcquire(parent_channel->getMutex(), osWaitForever) != osOK) return false;
    bool was_ready = parent_channel->hasWriterWaiting();
    parent_channel->getWaitingInAlt().clear();
    osMutexRelease(parent_channel->getMutex());
    return was_ready;
}

// --- ChanOutGuard Implementation ---

bool ChanOutGuard::enable(AltScheduler* alt, uint32_t bit) {
    if (osMutexAcquire(parent_channel->getMutex(), osWaitForever) != osOK) return false;

    if (parent_channel->hasReaderWaiting()) {
        osMutexRelease(parent_channel->getMutex());
        return true;
    }

    parent_channel->getWaitingOutAlt().set(alt, bit, const_cast<void*>(user_data_source), data_size);
    osMutexRelease(parent_channel->getMutex());
    return false;
}

void ChanOutGuard::activate() {
    if (osMutexAcquire(parent_channel->getMutex(), osWaitForever) != osOK) return;

    osThreadId_t receiver = parent_channel->getWaitingInTask();
    if (receiver != nullptr) {
        if (parent_channel->getNonAltInDataPtr() && user_data_source)
            memcpy(parent_channel->getNonAltInDataPtr(), user_data_source, data_size);

        parent_channel->clearWaitingIn();
        osMutexRelease(parent_channel->getMutex());
        osThreadFlagsSet(receiver, RENDEZVOUS_FLAG);
        return;
    }

    WaitingAlt& in_alt = parent_channel->getWaitingInAlt();
    if (in_alt.isActive()) {
        AltScheduler* partner_alt = in_alt.alt_ptr;
        uint32_t      partner_bit = in_alt.assigned_bit;

        if (in_alt.data_ptr && user_data_source)
            memcpy(in_alt.data_ptr, user_data_source, data_size);

        in_alt.clear();
        osMutexRelease(parent_channel->getMutex());
        if (partner_alt) partner_alt->wakeUp(partner_bit);
        return;
    }

    osMutexRelease(parent_channel->getMutex());
}

bool ChanOutGuard::disable() {
    if (osMutexAcquire(parent_channel->getMutex(), osWaitForever) != osOK) return false;
    bool was_ready = parent_channel->hasReaderWaiting();
    parent_channel->getWaitingOutAlt().clear();
    osMutexRelease(parent_channel->getMutex());
    return was_ready;
}

} // namespace csp::internal
