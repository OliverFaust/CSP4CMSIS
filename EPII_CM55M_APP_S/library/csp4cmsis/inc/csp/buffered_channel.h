#ifndef CSP4CMSIS_BUFFERED_CHANNEL_H
#define CSP4CMSIS_BUFFERED_CHANNEL_H

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "queue.h"
#include "channel_base.h" 
#include "alt.h"         
#include <cstdlib> 

namespace csp::internal {

    // Forward declarations of updated Guard classes
    template <typename T> class BufferedInputGuard;
    template <typename T> class BufferedOutputGuard;

    // =============================================================
    // CLASS 1: BufferedChannel<T> (Resident Guard version)
    // =============================================================
    template <typename T>
    class BufferedChannel : public internal::BaseAltChan<T>
    {
    private:
        QueueHandle_t queue_handle; 
        
        EventGroupHandle_t alt_read_group = NULL;
        EventBits_t alt_read_bit = 0;
        bool reader_is_alting = false;

        EventGroupHandle_t alt_write_group = NULL;
        EventBits_t alt_write_bit = 0;
        bool writer_is_alting = false;

        // RESIDENT GUARDS: Allocated statically as members of the channel
        BufferedInputGuard<T>  res_in_guard;
        BufferedOutputGuard<T> res_out_guard;
        
    public:
        BufferedChannel(size_t capacity) 
            : res_in_guard(this), res_out_guard(this) 
        {
            if (capacity == 0) std::abort(); 
            queue_handle = xQueueCreate(capacity, sizeof(T));
        }

        ~BufferedChannel() override {
            if (queue_handle) vQueueDelete(queue_handle);
        }

        // --- Core I/O ---
        void input(T* const dest) override {
            if (xQueueReceive(queue_handle, dest, portMAX_DELAY) == pdPASS) {
                taskENTER_CRITICAL();
                if (writer_is_alting) xEventGroupSetBits(alt_write_group, alt_write_bit);
                taskEXIT_CRITICAL();
            }
        }

        void output(const T* const source) override {
            if (xQueueSend(queue_handle, source, portMAX_DELAY) == pdPASS) {
                taskENTER_CRITICAL();
                if (reader_is_alting) xEventGroupSetBits(alt_read_group, alt_read_bit);
                taskEXIT_CRITICAL();
            }
        }

        void beginExtInput(T* const dest) override { this->input(dest); }
        void endExtInput() override { }
        
        // --- Updated Resident Guard Accessors ---
        Guard* getInputGuard(T& dest) override {
            res_in_guard.setTarget(&dest);
            return &res_in_guard;
        }

        Guard* getOutputGuard(const T& source) override {
            res_out_guard.setTarget(&source);
            return &res_out_guard;
        }
        
        // --- Internal Helpers ---
        bool pending() const { return uxQueueMessagesWaiting(queue_handle) > 0; } 
        bool space_available() const { return uxQueueSpacesAvailable(queue_handle) > 0; }
        
        void registerInputAlt(EventGroupHandle_t g, EventBits_t b) {
            taskENTER_CRITICAL(); reader_is_alting = true; alt_read_group = g; alt_read_bit = b; taskEXIT_CRITICAL();
        }
        void unregisterInputAlt() {
            taskENTER_CRITICAL(); reader_is_alting = false; taskEXIT_CRITICAL();
        }
        void registerOutputAlt(EventGroupHandle_t g, EventBits_t b) {
            taskENTER_CRITICAL(); writer_is_alting = true; alt_write_group = g; alt_write_bit = b; taskEXIT_CRITICAL();
        }
        void unregisterOutputAlt() {
            taskENTER_CRITICAL(); writer_is_alting = false; taskEXIT_CRITICAL();
        }

        QueueHandle_t getQueueHandle() const { return queue_handle; }
    };
    
    // =============================================================
    // CLASS 2: BufferedInputGuard (Late Binding)
    // =============================================================
    template <typename T>
    class BufferedInputGuard : public Guard {
    private:
        BufferedChannel<T>* channel;
        T* dest_ptr = nullptr; 
    public:
        BufferedInputGuard(BufferedChannel<T>* chan) : channel(chan) {}

        void setTarget(T* dest) { dest_ptr = dest; }

        bool enable(AltScheduler* alt, EventBits_t bit) override {
            if (channel->pending()) return true;
            channel->registerInputAlt(alt->getEventGroupHandle(), bit);
            return false;
        }

        bool disable() override {
            channel->unregisterInputAlt();
            return channel->pending();
        }

        void activate() override {
            xQueueReceive(channel->getQueueHandle(), dest_ptr, 0);
        }
    };
    
    // =============================================================
    // CLASS 3: BufferedOutputGuard (Late Binding)
    // =============================================================
    template <typename T>
    class BufferedOutputGuard : public Guard {
    private:
        BufferedChannel<T>* channel;
        const T* source_ptr = nullptr;
    public:
        BufferedOutputGuard(BufferedChannel<T>* chan) : channel(chan) {}

        void setTarget(const T* source) { source_ptr = source; }

        bool enable(AltScheduler* alt, EventBits_t bit) override {
            if (channel->space_available()) return true;
            channel->registerOutputAlt(alt->getEventGroupHandle(), bit);
            return false;
        }

        bool disable() override {
            channel->unregisterOutputAlt();
            return channel->space_available();
        }

        void activate() override {
            xQueueSend(channel->getQueueHandle(), source_ptr, 0);
        }
    };

} // namespace csp::internal

#endif