#ifndef ALT_CHANNEL_SYNC_H
#define ALT_CHANNEL_SYNC_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "alt.h"      
#include <cstdio> 

namespace csp::internal {

    class AltScheduler;

    struct WaitingAlt {
        AltScheduler* alt_ptr;
        EventBits_t assigned_bit;
        Guard* guard_ptr;
        void* data_ptr;
        size_t data_size;
        WaitingAlt() : alt_ptr(nullptr), assigned_bit(0), guard_ptr(nullptr), data_ptr(nullptr), data_size(0) {}
    };

    class AltChanSyncBase {
    protected:
        SemaphoreHandle_t mutex; 
        WaitingAlt waiting_in_alt;
        WaitingAlt waiting_out_alt;
        TaskHandle_t waiting_in_task;
        TaskHandle_t waiting_out_task;
        void* non_alt_in_data_ptr;
        const void* non_alt_out_data_ptr;

    public:
        AltChanSyncBase() : 
            mutex(nullptr), waiting_in_task(nullptr), waiting_out_task(nullptr),
            non_alt_in_data_ptr(nullptr), non_alt_out_data_ptr(nullptr) 
        {
            mutex = xSemaphoreCreateMutex();
        }

        virtual ~AltChanSyncBase() {
            if (mutex != nullptr) vSemaphoreDelete(mutex);
        }

        // --- Logic methods (Logic implemented in .cpp) ---
        // Note: These now assume the caller HAS ALREADY LOCKED the mutex
        bool tryHandshake(void* data_ptr, size_t size, bool is_writer);
        void registerWaitingTask(void* data_ptr, bool is_writer);
        
        void clearWaitingIn() { waiting_in_task = nullptr; non_alt_in_data_ptr = nullptr; }
        void clearWaitingOut() { waiting_out_task = nullptr; non_alt_out_data_ptr = nullptr; }

        SemaphoreHandle_t getMutex() { return mutex; }
        TaskHandle_t getWaitingInTask() const { return waiting_in_task; }
        TaskHandle_t getWaitingOutTask() const { return waiting_out_task; }
        void* getNonAltInDataPtr() const { return non_alt_in_data_ptr; }
        const void* getNonAltOutDataPtr() const { return non_alt_out_data_ptr; }
        WaitingAlt& getWaitingInAlt() { return waiting_in_alt; }
        WaitingAlt& getWaitingOutAlt() { return waiting_out_alt; }
    };

    // --- Guards ---
    class ChanInGuard : public Guard {
    private: 
        AltChanSyncBase* parent_channel;
        void* user_data_dest; 
        size_t data_size;
    public:
        ChanInGuard(AltChanSyncBase* parent, void* dest = nullptr, size_t size = 0) 
            : parent_channel(parent), user_data_dest(dest), data_size(size) {}
        bool enable(AltScheduler* alt, EventBits_t bit) override;
        bool disable() override;
        void activate() override;
        void updateBuffer(void* new_dest) { user_data_dest = new_dest; }
    };

    class ChanOutGuard : public Guard { 
    private: 
        AltChanSyncBase* parent_channel;
        const void* user_data_source; 
        size_t data_size;
    public:
        ChanOutGuard(AltChanSyncBase* parent, const void* src = nullptr, size_t size = 0) 
            : parent_channel(parent), user_data_source(src), data_size(size) {}
        bool enable(AltScheduler* alt, EventBits_t bit) override;
        bool disable() override;
        void activate() override;
        void updateBuffer(const void* new_src) { user_data_source = new_src; }
    };
}
#endif