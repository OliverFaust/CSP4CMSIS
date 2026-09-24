#ifndef CSP4CMSIS_SYNC_CHANNEL_H
#define CSP4CMSIS_SYNC_CHANNEL_H

#include "channel_base.h"
#include "alt.h"
#include "cmsis_os2.h"

namespace csp::internal {

    // Forward declaration with template param
    template <csp::BufferPolicy P> class SyncChannel;

    /**
     * @brief Guard for receiving from a synchronous channel.
     */
    template <csp::BufferPolicy P>
    class SyncChannelInputGuard : public Guard {
    private:
        SyncChannel<P>* channel = nullptr;
        void* user_data_dest = nullptr;
        size_t data_size = 0;
    public:
        SyncChannelInputGuard(SyncChannel<P>* chan) : channel(chan) {}
        void bind(void* dest, size_t size) { user_data_dest = dest; data_size = size; }

        bool enable(AltScheduler* alt, uint32_t bit) override;
        bool disable() override;
        void activate() override;
    };

    /**
     * @brief Guard for sending to a synchronous channel.
     */
    template <csp::BufferPolicy P>
    class SyncChannelOutputGuard : public Guard {
    private:
        SyncChannel<P>* channel = nullptr;
        const void* user_data_source = nullptr;
        size_t data_size = 0;
    public:
        SyncChannelOutputGuard(SyncChannel<P>* chan) : channel(chan) {}
        void bind(const void* src, size_t size) { user_data_source = src; data_size = size; }

        bool enable(AltScheduler* alt, uint32_t bit) override;
        bool disable() override;
        void activate() override;
    };

    /**
     * @brief Synchronous Signal Channel (void data).
     * Template policy P allows for Blocking (Standard CSP) or Sampling (KeepNewest/Oldest).
     */
    template <csp::BufferPolicy P>
    class SyncChannel : public internal::BaseAltChan<void> {
    public:
        enum State { IDLE, SENDER_WAITING, RECEIVER_WAITING };

    private:
        osMutexId_t mutex;
        State state;
        const void* data_ptr = nullptr;
        size_t data_len = 0;

        AltScheduler* waiting_alt_in = nullptr;
        uint32_t waiting_alt_bit_in = 0;
        SyncChannelInputGuard<P>* waiting_guard_in = nullptr;

        AltScheduler* waiting_alt_out = nullptr;
        uint32_t waiting_alt_bit_out = 0;
        SyncChannelOutputGuard<P>* waiting_guard_out = nullptr;

        // API 1.4 (CMSIS-RTOS2 migration): FreeRTOS's original design used
        // a zero-item-size xQueueCreate(1, 0) purely as a binary
        // rendezvous signal (no actual payload ever moves through it --
        // data is copied separately via data_ptr/memcpy). CMSIS-RTOS2's
        // message-queue API assumes a nonzero message size, so a binary
        // semaphore (osSemaphoreNew(1, 0, ...)) replaces it here instead
        // of forcing a zero-byte message through osMessageQueueNew() --
        // functionally identical (a pure "release one waiter" pulse), and
        // avoids relying on unspecified zero-size-message behavior. Not a
        // 1:1 rename; see the migration report for why.
        osSemaphoreId_t sender_sem;
        osSemaphoreId_t receiver_sem;

        SyncChannelInputGuard<P>  res_in_guard;
        SyncChannelOutputGuard<P> res_out_guard;

    public:
        // Constructor and Destructor bodies removed (they are defined in sync_channel.cpp)
        SyncChannel();
        ~SyncChannel() override;

        // --- Core Logic ---
        void reset(); // Required by .cpp implementation

        bool pending() override;
        bool space_available() override;
        bool putFromISR() override;

        internal::Guard* getInputGuard() override { return &res_in_guard; }
        internal::Guard* getOutputGuard() override { return &res_out_guard; }

        void input(void* const dest) override;
        void output(const void* const source) override;

        void beginExtInput(void* const dest) override { input(dest); }
        void endExtInput() override {}

        // --- Alt Registration helpers ---
        bool registerAltIn(AltScheduler* alt, uint32_t bit, SyncChannelInputGuard<P>* guard);
        bool unregisterAltIn(AltScheduler* alt);
        bool registerAltOut(AltScheduler* alt, uint32_t bit, SyncChannelOutputGuard<P>* guard);
        bool unregisterAltOut(AltScheduler* alt);

        // --- Getters ---
        osMutexId_t getMutex() const { return mutex; }
        State getState() const { return state; }
        osSemaphoreId_t getSenderSem() const { return sender_sem; }
        osSemaphoreId_t getReceiverSem() const { return receiver_sem; }
        const void* getDataPtr() const { return data_ptr; } // Required by Guards
    };

} // namespace csp::internal

#endif // CSP4CMSIS_SYNC_CHANNEL_H
