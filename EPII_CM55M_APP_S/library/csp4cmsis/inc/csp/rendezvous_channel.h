#ifndef CSP4CMSIS_RENDEZVOUS_CHANNEL_H
#define CSP4CMSIS_RENDEZVOUS_CHANNEL_H

#include "channel_base.h"       
#include "alt_channel_sync.h"   
#include "FreeRTOS.h"
#include "task.h"               
#include <cstring>              

namespace csp::internal {

template <typename T>
class RendezvousChannel : public BaseAltChan<T> {
private:
    AltChanSyncBase sync_base;
    
    // RESIDENT GUARDS: These are now permanent members of the channel.
    // Memory is allocated once when the channel is created (ideally as static).
    internal::ChanInGuard  res_in_guard;
    internal::ChanOutGuard res_out_guard; 

public:
    RendezvousChannel() 
        : res_in_guard(&sync_base, nullptr, sizeof(T)),
          res_out_guard(&sync_base, nullptr, sizeof(T)) {}

    virtual ~RendezvousChannel() override = default;

    // --- Blocking Input (Receiver) ---
    virtual void input(T* const dest) override {
        xTaskNotifyStateClear(NULL);

        // 1. Try to find an existing Sender
        if (sync_base.tryRendezvous((void*)dest, sizeof(T), false)) {
            if (sync_base.getWaitingOutTask() != nullptr) {
                // Partner found (blocking sender)! Perform the transfer.
                memcpy(dest, sync_base.getNonAltOutDataPtr(), sizeof(T));
                TaskHandle_t sender = sync_base.getWaitingOutTask();
                sync_base.clearWaitingOut();
                xTaskNotifyGive(sender);
                return;
            }
            // If tryRendezvous returned true but no waiting task, it matched an ALT sender.
            // In that case, the ALT sender's activate() will handle the notification.
        }

        // 2. No sender ready? Register ourselves and wait.
        sync_base.registerBlockingTask((void*)dest, false);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    // --- Blocking Output (Sender) ---
    virtual void output(const T* const source) override {
        xTaskNotifyStateClear(NULL);

        // 1. Try to find a Receiver
        if (sync_base.tryRendezvous((void*)source, sizeof(T), true)) {
            // If we hit an ALT Receiver or a waiting blocking Receiver, 
            // we wait for the handshake to complete.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            return;
        }

        // 2. No receiver? Register buffer and wait for Receiver to "pull" the data
        sync_base.registerBlockingTask((void*)const_cast<T*>(source), true);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    // --- Resident Guard Implementation ---
    // These override the new BaseAltChan interface
    
    virtual internal::Guard* getInputGuard(T& dest) override {
        res_in_guard.updateBuffer(&dest); 
        return &res_in_guard;
    }
    
    virtual internal::Guard* getOutputGuard(const T& source) override { 
        // Cast away constness only for the internal transfer buffer pointer
        res_out_guard.updateBuffer(const_cast<void*>(static_cast<const void*>(&source)));
        return &res_out_guard; 
    }
    
    virtual bool pending() {
        if (xSemaphoreTake(sync_base.getMutex(), 0) == pdTRUE) {
            bool has_partner = (sync_base.getWaitingInTask() != nullptr) || 
                               (sync_base.getWaitingOutTask() != nullptr);
            xSemaphoreGive(sync_base.getMutex());
            return has_partner;
        }
        return false;
    }
    
    virtual void beginExtInput(T* const dest) override {}
    virtual void endExtInput() override {}
};

} // namespace csp::internal

#endif