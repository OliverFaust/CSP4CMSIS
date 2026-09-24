#ifndef CSP4CMSIS_BUFFERED_CHANNEL_H
#define CSP4CMSIS_BUFFERED_CHANNEL_H

#include "cmsis_os2.h"
#include "csp_critical.h"
#include "channel_base.h"
#include "alt.h"
#include <cstdlib>

namespace csp::internal {

    template <typename T, csp::BufferPolicy P> class BufferedInputGuard;
    template <typename T, csp::BufferPolicy P> class BufferedOutputGuard;

    /**
     * @brief A policy-based buffered channel implementation using CMSIS-RTOS2 message queues.
     */
    template <typename T, csp::BufferPolicy P = csp::BufferPolicy::Block>
    class BufferedChannel : public internal::BaseAltChan<T>
    {
    private:
        osMessageQueueId_t queue_handle;

        AltScheduler* alt_reader = nullptr;
        uint32_t      read_bit = 0;

        AltScheduler* alt_writer = nullptr;
        uint32_t      write_bit = 0;

        BufferedInputGuard<T, P>  res_in_guard;
        BufferedOutputGuard<T, P> res_out_guard;

        /** @brief Internal helper to notify an ALTed reader that data is available. */
        void _notifyReader() {
            uint32_t saved = csp_enter_critical();
            if (alt_reader != nullptr) {
                alt_reader->wakeUp(read_bit);
            }
            csp_exit_critical(saved);
        }

        /** @brief Internal helper to notify an ALTed writer that space is available. */
        void _notifyWriter() {
            uint32_t saved = csp_enter_critical();
            if (alt_writer != nullptr) {
                alt_writer->wakeUp(write_bit);
            }
            csp_exit_critical(saved);
        }

    public:
        BufferedChannel(size_t capacity)
            : res_in_guard(this), res_out_guard(this)
        {
            if (capacity == 0) std::abort();
            queue_handle = osMessageQueueNew(capacity, sizeof(T), NULL);
        }

        ~BufferedChannel() override {
            if (queue_handle) osMessageQueueDelete(queue_handle);
        }

        // --- BaseAltChan Overrides ---

        bool pending() override {
            return osMessageQueueGetCount(queue_handle) > 0;
        }

        /**
         * @brief Checks if a write operation will block.
         * For KeepNewest/KeepOldest, this always returns true as they are non-blocking.
         */
        bool space_available() override {
            if constexpr (P == csp::BufferPolicy::Block) {
                return osMessageQueueGetSpace(queue_handle) > 0;
            }
            return true;
        }

        /**
         * @brief Policy-aware ISR output.
         * Useful for high-speed peripherals like the STM32N6 DCMIPP (Camera).
         *
         * osMessageQueuePut()/osMessageQueueGet() detect IRQ context
         * internally (timeout must be 0 from an ISR, satisfied below) --
         * unlike FreeRTOS's separate xQueueSendFromISR()/
         * xQueueReceiveFromISR() plus an explicit portYIELD_FROM_ISR(),
         * one call each replaces both context's variants.
         */
        bool putFromISR(const T& data) override {
            bool result = false;

            if (osMessageQueuePut(queue_handle, &data, 0, 0) == osOK) {
                result = true;
            } else {
                if constexpr (P == csp::BufferPolicy::KeepNewest) {
                    T dummy;
                    osMessageQueueGet(queue_handle, &dummy, NULL, 0);
                    osMessageQueuePut(queue_handle, &data, 0, 0);
                    result = true;
                } else if constexpr (P == csp::BufferPolicy::KeepOldest) {
                    result = true; // Handled by discarding
                }
            }

            if (result && alt_reader != nullptr) {
                alt_reader->wakeUp(read_bit);
            }

            return result;
        }

        // --- Core I/O ---

        void input(T* const dest) override {
            if (osMessageQueueGet(queue_handle, dest, NULL, osWaitForever) == osOK) {
                _notifyWriter();
            }
        }

        void output(const T* const source) override {
            if constexpr (P == csp::BufferPolicy::Block) {
                if (osMessageQueuePut(queue_handle, source, 0, osWaitForever) == osOK) {
                    _notifyReader();
                }
            } else {
                // Non-blocking branch: 0 timeout
                if (osMessageQueuePut(queue_handle, source, 0, 0) != osOK) {
                    if constexpr (P == csp::BufferPolicy::KeepNewest) {
                        T dummy;
                        osMessageQueueGet(queue_handle, &dummy, NULL, 0); // Drop oldest
                        osMessageQueuePut(queue_handle, source, 0, 0);    // Push newest
                    }
                    // KeepOldest does nothing
                }
                _notifyReader();
            }
        }

        void beginExtInput(T* const dest) override { this->input(dest); }
        void endExtInput() override { }

        // --- Guard Factories ---

        internal::Guard* getInputGuard(T& dest) override {
            res_in_guard.setTarget(&dest);
            return &res_in_guard;
        }

        internal::Guard* getOutputGuard(const T& source) override {
            res_out_guard.setTarget(&source);
            return &res_out_guard;
        }

        // --- ALT Registration ---

        void registerInputAlt(AltScheduler* alt, uint32_t b) {
            uint32_t saved = csp_enter_critical(); alt_reader = alt; read_bit = b; csp_exit_critical(saved);
        }
        void unregisterInputAlt() {
            uint32_t saved = csp_enter_critical(); alt_reader = nullptr; csp_exit_critical(saved);
        }
        void registerOutputAlt(AltScheduler* alt, uint32_t b) {
            uint32_t saved = csp_enter_critical(); alt_writer = alt; write_bit = b; csp_exit_critical(saved);
        }
        void unregisterOutputAlt() {
            uint32_t saved = csp_enter_critical(); alt_writer = nullptr; csp_exit_critical(saved);
        }

        osMessageQueueId_t getQueueHandle() const { return queue_handle; }
    };

    // =============================================================
    // Guards
    // =============================================================

    template <typename T, csp::BufferPolicy P>
    class BufferedInputGuard : public Guard {
    private:
        BufferedChannel<T, P>* channel;
        T* dest_ptr = nullptr;
    public:
        BufferedInputGuard(BufferedChannel<T, P>* chan) : channel(chan) {}
        void setTarget(T* dest) { dest_ptr = dest; }

        bool enable(AltScheduler* alt, uint32_t bit) override {
            if (channel->pending()) return true;
            channel->registerInputAlt(alt, bit);
            return false;
        }
        bool disable() override {
            channel->unregisterInputAlt();
            return channel->pending();
        }
        void activate() override {
            osMessageQueueGet(channel->getQueueHandle(), dest_ptr, NULL, 0);
        }
    };

    template <typename T, csp::BufferPolicy P>
    class BufferedOutputGuard : public Guard {
    private:
        BufferedChannel<T, P>* channel;
        const T* source_ptr = nullptr;
    public:
        BufferedOutputGuard(BufferedChannel<T, P>* chan) : channel(chan) {}
        void setTarget(const T* source) { source_ptr = source; }

        bool enable(AltScheduler* alt, uint32_t bit) override {
            // KeepNewest/KeepOldest are always ready to accept output
            if (channel->space_available()) return true;

            channel->registerOutputAlt(alt, bit);
            return false;
        }
        bool disable() override {
            if constexpr (P != csp::BufferPolicy::Block) return true;
            channel->unregisterOutputAlt();
            return channel->space_available();
        }
        void activate() override {
            channel->output(source_ptr);
        }
    };

} // namespace csp::internal

#endif // CSP4CMSIS_BUFFERED_CHANNEL_H
