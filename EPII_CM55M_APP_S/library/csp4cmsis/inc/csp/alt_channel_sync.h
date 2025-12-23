#ifndef ALT_CHANNEL_SYNC_H
#define ALT_CHANNEL_SYNC_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "event_groups.h" 
#include "alt.h"      

namespace csp {
    /**
     * @brief Glue logic for Pipe Syntax (chan | msg).
     * This struct maps a Channel + Message pair to a Guard.
     */
    template <typename T, typename ChanType>
    struct ChannelBinding {
        ChanType& channel;
        T& data_ref;

        ChannelBinding(ChanType& c, T& d) : channel(c), data_ref(d) {}

        // This is the missing method the compiler is looking for!
        internal::Guard* getInternalGuard() const {
            // This calls the virtual getInputGuard/getOutputGuard 
            // defined in your RendezvousChannel or BufferedChannel.
            return channel.getGuard(data_ref); 
        }
    };
}

namespace csp::internal {

    class AltScheduler;

    /**
     * @brief Represents a process waiting on an ALT operation.
     */
    struct WaitingAlt {
        AltScheduler* alt_ptr;
        EventBits_t assigned_bit;
        Guard* guard_ptr;
        void* data_ptr;
        size_t data_size;
    };

    /**
     * @brief Base synchronization logic shared by Rendezvous channels.
     * Manages state for both blocking tasks and ALT-ing tasks.
     */
    class AltChanSyncBase {
    protected:
        SemaphoreHandle_t mutex; 
        WaitingAlt waiting_in_alt;
        WaitingAlt waiting_out_alt;
        
        TaskHandle_t waiting_in_task = nullptr;
        TaskHandle_t waiting_out_task = nullptr;
        
        void* non_alt_in_data_ptr = nullptr;
        const void* non_alt_out_data_ptr = nullptr;

    public:
        AltChanSyncBase();
        virtual ~AltChanSyncBase();

        // Core Handshake Logic (Implemented in alt_channel_sync.cpp)
        Guard* tryRendezvous(void* data_ptr, size_t size, bool is_writer);
        bool registerBlockingTask(void* data_ptr, bool is_writer);
        
        void clearWaitingIn();
        void clearWaitingOut();

        // Getters
        SemaphoreHandle_t getMutex() const { return mutex; }
        TaskHandle_t getWaitingInTask() const { return waiting_in_task; }
        TaskHandle_t getWaitingOutTask() const { return waiting_out_task; }
        
        void* getNonAltInDataPtr() const { return non_alt_in_data_ptr; }
        const void* getNonAltOutDataPtr() const { return non_alt_out_data_ptr; }

        WaitingAlt& getWaitingInAlt() { return waiting_in_alt; }
        WaitingAlt& getWaitingOutAlt() { return waiting_out_alt; }
    };

    // --- Channel Guards (Resident Mode) ---

    /**
     * @brief Resident Guard for Rendezvous Input.
     */
    class ChanInGuard : public Guard {
    private: 
        AltChanSyncBase* parent_channel;
        void* user_data_dest; 
        size_t data_size;
        
    public:
        // Constructor no longer requires initial dest pointer
        ChanInGuard(AltChanSyncBase* parent, void* dest = nullptr, size_t size = 0) 
            : parent_channel(parent), user_data_dest(dest), data_size(size) {}

        bool enable(AltScheduler* alt, EventBits_t bit) override;
        bool disable() override;
        void activate() override;

        /**
         * @brief Late-binding of user data buffer.
         * Called by the Pipe Syntax (in | msg) to configure the resident guard.
         */
        void updateBuffer(void* new_dest) { user_data_dest = new_dest; }
        virtual ~ChanInGuard() override = default;
    };

    /**
     * @brief Resident Guard for Rendezvous Output.
     */
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

        /**
         * @brief Late-binding of user data source.
         */
        void updateBuffer(const void* new_src) { user_data_source = new_src; }
        virtual ~ChanOutGuard() override = default;
    };
    
} // namespace csp::internal



#endif // ALT_CHANNEL_SYNC_H