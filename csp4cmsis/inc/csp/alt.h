#ifndef CSP4CMSIS_ALT_H
#define CSP4CMSIS_ALT_H

#include "cmsis_os2.h"
#include "csp_rtos_static.h"
#include <stddef.h>
#include <initializer_list>
#include <type_traits>
#include "time.h"
#include "csp_fatal.h"

// =============================================================================
// Thread-flag allocation (CMSIS-RTOS2 thread flags, per thread)
//
//   bit  0       RENDEZVOUS_FLAG (alt_channel_sync.h): plain blocking rendezvous
//   bits 8..23   ALT wakeups: guard i of the Alternative a thread is currently
//                selecting on is signalled with bit (8 + i); MAX_GUARDS = 16
//   bits 1..7,
//   bits 24..30  free for the application
//   bit  31      invalid in CMSIS-RTOS2 (error-code range)
//
// select() clears its bits on entry and re-verifies every wakeup with the
// guard (confirm()), so a late signal from an earlier select() is harmless.
//
// FreeRTOS backend: the CMSIS-RTOS2 adapter implements thread flags with the
// task notification at index 0 (xTaskNotify/xTaskNotifyWait). Native FreeRTOS
// code that uses index-0 notifications on the same task (xTaskNotifyGive,
// ulTaskNotifyTake, stream/message buffers, ...) collides with CSP4CMSIS
// processes. Keep native notifications off CSP process threads, or move them
// to another index (configTASK_NOTIFICATION_ARRAY_ENTRIES > 1).
// =============================================================================
#define CSP4CMSIS_ALT_FLAG_SHIFT   8U
#define CSP4CMSIS_ALT_MAX_GUARDS   16U

namespace csp {
    // Forward declarations
    template <typename T> class Chanin;
    template <typename T> class Chanout;

    namespace internal {
        class AltScheduler;

        constexpr uint32_t ALT_FLAG_SHIFT = CSP4CMSIS_ALT_FLAG_SHIFT;
        constexpr uint32_t ALT_MAX_GUARDS = CSP4CMSIS_ALT_MAX_GUARDS;
        constexpr uint32_t ALT_FLAG_MASK  = ((1UL << ALT_MAX_GUARDS) - 1UL) << ALT_FLAG_SHIFT;
        /// Thread-flag bit used to wake the selecting thread for guard index i.
        constexpr uint32_t altFlag(size_t i) { return 1UL << (ALT_FLAG_SHIFT + i); }

        /**
         * @brief Base Guard Interface.
         *
         * select() protocol, per round:
         *   1. enable(alt, flag) in fairness order until one returns true
         *      (ready now). A guard that returns false registers so that its
         *      partner signals `flag` to alt->ownerThread() when it may be
         *      ready. enable() must check and register atomically.
         *   2. If none was ready: wait for any enabled guard's flag.
         *   3. disable() every guard that was enabled this round (and only
         *      those); it returns whether the guard can complete now.
         *   4. confirm(disable_result) on the chosen guard: false = stale
         *      wakeup, start a new round.
         *   5. activate(): commit. false = a competitor took the item/space
         *      between disable() and activate(); start a new round.
         */
        class Guard {
        public:
            virtual bool enable(AltScheduler* alt, uint32_t flag) = 0;
            virtual bool disable() = 0;
            /// Default: trust the wakeup (guards whose completion cannot be
            /// re-checked after the partner has already transferred data,
            /// e.g. ALT-vs-ALT rendezvous).
            virtual bool confirm(bool disable_result) { (void)disable_result; return true; }
            virtual bool activate() = 0;
            virtual ~Guard() = default;
        };

        class AltScheduler {
        private:
            // Thread currently running select() on this ALT; wakeups are
            // thread flags sent to it (no RTOS object per Alternative).
            osThreadId_t owner = nullptr;
        public:
            AltScheduler() = default;
            AltScheduler(const AltScheduler&) = delete;
            AltScheduler& operator=(const AltScheduler&) = delete;

            /**
             * @brief The core ALT selection logic (see Guard).
             * @param offset Used for Fair Alts to prevent starvation.
             * @return The index of the selected guard.
             */
            unsigned int select(Guard** guardArray, size_t amount, size_t offset = 0);

            /// Thread flag for `flag` to the selecting thread (ISR-safe).
            /// Callers must not hold a CSP critical section.
            void wakeUp(uint32_t flag);
            osThreadId_t ownerThread() const { return owner; }
        };

        class TimerGuard : public Guard {
        private:
            uint32_t delay_ticks;
            osTimerId_t timer_handle;
            // Snapshot of whom to wake, taken in enable(): the callback never
            // dereferences the (possibly already finished) Alternative.
            osThreadId_t volatile wake_thread;
            uint32_t     volatile wake_flag;
            bool         volatile fired;
#if defined(CSP4CMSIS_STATIC_ALLOCATION)
            // Static osTimer control block (no RTOS heap); see csp_rtos_static.h
            // for the backend-specific size rules.
            csp_static_timer_storage_t timer_storage;
#endif
            // CMSIS-RTOS2 passes the argument given to osTimerNew() straight
            // to the callback -- no TimerHandle_t-to-owner lookup needed,
            // unlike FreeRTOS's pvTimerGetTimerID(). Genuine simplification,
            // not just a rename.
            static void TimerCallback(void* argument);
        public:
            TimerGuard(csp::Time delay);
            ~TimerGuard() override;
            // The osTimer is bound to `this` (callback argument, static
            // control block): copies would share or dangle.
            TimerGuard(const TimerGuard&) = delete;
            TimerGuard& operator=(const TimerGuard&) = delete;
            bool enable(AltScheduler* alt, uint32_t flag) override;
            bool disable() override;                       // returns: expired
            bool confirm(bool expired) override { return expired; }
            bool activate() override { return true; }
        };

        /**
         * @brief A Skip Guard (Always ready).
         * Used internally when a Channel Policy is non-blocking.
         */
        class SkipGuard : public Guard {
        public:
            bool enable(AltScheduler*, uint32_t) override { return true; }
            bool disable() override { return true; }
            bool activate() override { return true; }
        };

    } // namespace internal

    /**
     * @brief Glue logic for Pipe Syntax (chan | msg).
     */
    template <typename T, typename ChanType>
    struct ChannelBinding {
        ChanType& channel;
        T& data_ref;

        ChannelBinding(ChanType& c, T& d) : channel(c), data_ref(d) {}

        internal::Guard* getInternalGuard() const {
            return channel.getGuard(data_ref);
        }
    };

    /**
     * @brief Public Wrapper for Guards.
     */
    class Guard {
    public:
        internal::Guard* internal_guard_ptr = nullptr;
        virtual ~Guard() = default;
    protected:
        Guard(internal::Guard* internal_ptr) : internal_guard_ptr(internal_ptr) {}
    };

    class RelTimeoutGuard : public Guard {
    private:
        internal::TimerGuard timer_storage;
    public:
        RelTimeoutGuard(csp::Time delay)
            : Guard(&timer_storage), timer_storage(delay) {}
        ~RelTimeoutGuard() override = default;
        // internal_guard_ptr points into this object: not copyable/movable.
        RelTimeoutGuard(const RelTimeoutGuard&) = delete;
        RelTimeoutGuard& operator=(const RelTimeoutGuard&) = delete;
    };

    /**
     * @brief The Alternative (ALT) construct.
     * Manages multiple guards and selects the first one available.
     */
    class Alternative {
    private:
        static const size_t MAX_GUARDS = internal::ALT_MAX_GUARDS;
        internal::Guard* internal_guards[MAX_GUARDS];
        size_t num_guards = 0;
        internal::AltScheduler internal_alt;
        size_t fair_select_start_index = 0;

    public:
        Alternative() : num_guards(0) {}

        // Bindings are taken by reference (never copied): copying a
        // RelTimeoutGuard would duplicate its osTimer. The constraint keeps
        // this template from hijacking copy construction.
        template <typename... Bindings,
                  typename = std::enable_if_t<(sizeof...(Bindings) > 0) &&
                      !(std::is_same_v<std::decay_t<Bindings>, Alternative> || ...)>>
        Alternative(Bindings&&... bindings) : num_guards(0) {
            (addBinding(bindings), ...);
        }

        Alternative(std::initializer_list<internal::Guard*> guard_list);
        Alternative(std::initializer_list<csp::Guard*> guard_list);

        /**
         * @brief Priority Select: Always checks guards in the order they were added.
         */
        int priSelect();

        /**
         * @brief Fair Select: Rotates the starting index to ensure all guards get a turn.
         */
        int fairSelect();

        // --- Binding Helpers ---

        template <typename T>
        void addBinding(const ChannelBinding<T, Chanin<T>>& b) {
            if (num_guards < MAX_GUARDS) {
                internal_guards[num_guards++] = b.getInternalGuard();
            }
        }

        template <typename T>
        void addBinding(const ChannelBinding<const T, Chanout<T>>& b) {
            if (num_guards < MAX_GUARDS) {
                internal_guards[num_guards++] = b.getInternalGuard();
            }
        }

        void addBinding(RelTimeoutGuard& tg) {
            if (num_guards < MAX_GUARDS) {
                internal_guards[num_guards++] = tg.internal_guard_ptr;
            }
        }

        void addBinding(internal::Guard* g) {
            if (num_guards < MAX_GUARDS) {
                internal_guards[num_guards++] = g;
            }
        }
    };
}

#endif // CSP4CMSIS_ALT_H
