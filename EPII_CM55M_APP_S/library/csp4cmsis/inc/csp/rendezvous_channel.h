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
        const char* tname = pcTaskGetName(NULL);

        if (xSemaphoreTake(sync_base.getMutex(), portMAX_DELAY) == pdTRUE) {
            // Logic: Is a sender already waiting?
            if (sync_base.tryHandshake((void*)dest, sizeof(T), false)) {
                xSemaphoreGive(sync_base.getMutex());
                return; 
            }

            // No partner: Register our buffer and block
            sync_base.registerWaitingTask((void*)dest, false);
            xSemaphoreGive(sync_base.getMutex());
        }

        // Wait for notification from the sender (the "Second Arriver")
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    // --- Blocking Output (Sender) ---
    virtual void output(const T* const source) override {
        xTaskNotifyStateClear(NULL);
        const char* tname = pcTaskGetName(NULL);

        if (xSemaphoreTake(sync_base.getMutex(), portMAX_DELAY) == pdTRUE) {
            // Logic: Is a receiver already waiting?
            if (sync_base.tryHandshake((void*)const_cast<T*>(source), sizeof(T), true)) {
                xSemaphoreGive(sync_base.getMutex());
                return; 
            }

            // No partner: Register our source and block
            sync_base.registerWaitingTask((void*)const_cast<T*>(source), true);
            xSemaphoreGive(sync_base.getMutex());
        }

        // Wait for notification from the receiver
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    // --- Resident Guard Implementation (For ALT support) ---
    virtual internal::Guard* getInputGuard(T& dest) override {
        res_in_guard.updateBuffer(&dest); 
        return &res_in_guard;
    }
    
    virtual internal::Guard* getOutputGuard(const T& source) override { 
        res_out_guard.updateBuffer(const_cast<void*>(static_cast<const void*>(&source)));
        return &res_out_guard; 
    }
    
    virtual bool pending() {
        bool has_partner = false;
        if (xSemaphoreTake(sync_base.getMutex(), 0) == pdTRUE) {
            has_partner = (sync_base.getWaitingInTask() != nullptr) || 
                          (sync_base.getWaitingOutTask() != nullptr);
            xSemaphoreGive(sync_base.getMutex());
        }
        return has_partner;
    }
    
    virtual void beginExtInput(T* const dest) override {}
    virtual void endExtInput() override {}
};

} // namespace csp::internal

#endif