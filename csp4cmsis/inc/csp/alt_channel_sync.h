#ifndef ALT_CHANNEL_SYNC_H
#define ALT_CHANNEL_SYNC_H

#include "cmsis_os2.h"
#include "alt.h"
#include <cstdio>

namespace csp::internal {

    class AltScheduler;

    // Thread-flag bit used for the plain (non-ALT) blocking rendezvous
    // handshake in both this file's Guards (alt_channel_sync.cpp) and
    // RendezvousChannel's own direct input()/output() path
    // (rendezvous_channel.h) -- osThreadFlagsSet()/osThreadFlagsWait()
    // replace FreeRTOS's xTaskNotifyGive()/ulTaskNotifyTake(), which used
    // an opaque per-task notification count rather than a specific bit.
    // Any single bit works; shared here (rather than each call site
    // defining its own) since both files need the exact same value on
    // both the setting and waiting side of the handshake.
    static constexpr uint32_t RENDEZVOUS_FLAG = 0x00000001U;

    /**
     * @brief Represents an Alternative (ALT) operation currently waiting on a channel.
     */
    struct WaitingAlt {
        AltScheduler* alt_ptr;
        uint32_t assigned_bit;
        void* data_ptr;
        size_t data_size;

        WaitingAlt() : alt_ptr(nullptr), assigned_bit(0), data_ptr(nullptr), data_size(0) {}

        void set(AltScheduler* a, uint32_t b, void* d, size_t s) {
            alt_ptr = a;
            assigned_bit = b;
            data_ptr = d;
            data_size = s;
        }

        void clear() {
            alt_ptr = nullptr;
            assigned_bit = 0;
            data_ptr = nullptr;
            data_size = 0;
        }

        bool isActive() const { return alt_ptr != nullptr; }
    };

    /**
     * @brief Base synchronization primitive for channels supporting ALT.
     * Modified to provide explicit partner-status checks for sampling policies.
     */
    class AltChanSyncBase {
    protected:
        osMutexId_t mutex;

        // Slots for processes currently blocked in an Alternative (ALT) select
        WaitingAlt waiting_in_alt;
        WaitingAlt waiting_out_alt;

        // Slots for standard blocking processes (input() / output())
        osThreadId_t waiting_in_task;
        osThreadId_t waiting_out_task;
        void* non_alt_in_data_ptr;
        const void* non_alt_out_data_ptr;

    public:
        AltChanSyncBase();
        virtual ~AltChanSyncBase();

        /**
         * @brief Checks if a partner is ready to communicate right now.
         * Essential for KeepNewest/KeepOldest policies in Rendezvous.
         */
        bool hasReaderWaiting() const {
            return (waiting_in_task != nullptr) || waiting_in_alt.isActive();
        }

        bool hasWriterWaiting() const {
            return (waiting_out_task != nullptr) || waiting_out_alt.isActive();
        }

        // Perform or verify a rendezvous
        bool tryHandshake(void* data_ptr, size_t size, bool is_writer);

        // Register a standard task for blocking I/O
        void registerWaitingTask(void* data_ptr, bool is_writer);

        void clearWaitingIn() { waiting_in_task = nullptr; non_alt_in_data_ptr = nullptr; }
        void clearWaitingOut() { waiting_out_task = nullptr; non_alt_out_data_ptr = nullptr; }

        // Getters
        osMutexId_t getMutex() { return mutex; }
        osThreadId_t getWaitingInTask() const { return waiting_in_task; }
        osThreadId_t getWaitingOutTask() const { return waiting_out_task; }
        void* getNonAltInDataPtr() const { return non_alt_in_data_ptr; }
        const void* getNonAltOutDataPtr() const { return non_alt_out_data_ptr; }

        AltScheduler* getAltInScheduler() const { return waiting_in_alt.alt_ptr; }
        uint32_t      getAltInBit() const       { return waiting_in_alt.assigned_bit; }
        AltScheduler* getAltOutScheduler() const { return waiting_out_alt.alt_ptr; }
        uint32_t      getAltOutBit() const       { return waiting_out_alt.assigned_bit; }

        WaitingAlt& getWaitingInAlt() { return waiting_in_alt; }
        WaitingAlt& getWaitingOutAlt() { return waiting_out_alt; }
    };

    // =============================================================
    // Guards: Interfaces between Channels and the AltScheduler
    // =============================================================

    /**
     * @brief Input Guard for Rendezvous channels.
     */
    class ChanInGuard : public Guard {
    private:
        AltChanSyncBase* parent_channel;
        void* user_data_dest;
        size_t data_size;
    public:
        ChanInGuard(AltChanSyncBase* parent, void* dest = nullptr, size_t size = 0)
            : parent_channel(parent), user_data_dest(dest), data_size(size) {}

        bool enable(AltScheduler* alt, uint32_t bit) override;
        bool disable() override;
        void activate() override;
        void updateBuffer(void* new_dest) { user_data_dest = new_dest; }
    };

    /**
     * @brief Output Guard for Rendezvous channels.
     */
    class ChanOutGuard : public Guard {
    private:
        AltChanSyncBase* parent_channel;
        const void* user_data_source;
        size_t data_size;
    public:
        ChanOutGuard(AltChanSyncBase* parent, const void* src = nullptr, size_t size = 0)
            : parent_channel(parent), user_data_source(src), data_size(size) {}

        bool enable(AltScheduler* alt, uint32_t bit) override;
        bool disable() override;
        void activate() override;
        void updateBuffer(const void* new_src) { user_data_source = (void*)new_src; }
    };
}
#endif
