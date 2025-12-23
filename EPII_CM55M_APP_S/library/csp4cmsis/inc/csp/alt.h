#ifndef CSP4CMSIS_ALT_H
#define CSP4CMSIS_ALT_H

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "event_groups.h"
#include <stddef.h> 
#include <initializer_list>
#include "time.h" 

namespace csp {
    // Forward declarations for Pipe Syntax support
    template <typename T, typename ChanType> struct ChannelBinding;
    template <typename T> class Chanin;
    template <typename T> class Chanout;

    namespace internal {
        class AltScheduler; 

        /**
         * @brief Base Guard Interface.
         */
        class Guard {
        public:
            virtual bool enable(AltScheduler* alt, EventBits_t bit) = 0;
            virtual bool disable() = 0;
            virtual void activate() = 0;
            virtual ~Guard() = default;
        };

        class AltScheduler {
        private:
            TaskHandle_t waiting_task_handle = nullptr;
            EventGroupHandle_t event_group = nullptr;
        public:
            AltScheduler();
            ~AltScheduler(); 
            void initForCurrentTask(); 
            unsigned int select(Guard** guardArray, size_t amount, size_t offset = 0);
            void wakeUp(EventBits_t bit); 
            EventGroupHandle_t getEventGroupHandle() const { return event_group; }
        };

        class TimerGuard : public Guard {
        private:
            AltScheduler* parent_alt;
            TickType_t delay_ticks;
            TimerHandle_t timer_handle;
            EventBits_t assigned_bit; 
            static void TimerCallback(TimerHandle_t xTimer);
        public:
            TimerGuard(csp::Time delay);
            ~TimerGuard() override;
            bool enable(AltScheduler* alt, EventBits_t bit) override;
            bool disable() override; 
            void activate() override; 
        };
    } // namespace internal

    /**
     * @brief Public Wrapper for Guards to resolve naming conflicts.
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
    };
} 

// Include this AFTER the base Guard and Scheduler are defined
// so ChannelBinding can see the internal::Guard definition.
#include "alt_channel_sync.h"

namespace csp {
    class Alternative {
    private:
        static const size_t MAX_GUARDS = 16;
        internal::Guard* internal_guards[MAX_GUARDS];
        size_t num_guards = 0;
        internal::AltScheduler internal_alt; 
        size_t fair_select_start_index = 0; 
        
    public:
        Alternative() : num_guards(0) {}
        ~Alternative() = default;

        /**
         * @brief Variadic constructor to allow Alternative alt(in1, in2, timer);
         */
        template <typename... Bindings>
        Alternative(Bindings... bindings) : num_guards(0) {
            (addBinding(bindings), ...);
        }

        // Standard constructors for list initialization
        Alternative(std::initializer_list<internal::Guard*> guard_list);
        Alternative(std::initializer_list<csp::Guard*> guard_list);

        int priSelect();  
        int fairSelect(); 

    private:
        /**
         * @brief Binding helpers. 
         * Definitions must remain in the header so the compiler can 
         * generate code for specific types (e.g. Message) during compilation.
         */
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
    };
}

#endif // CSP4CMSIS_ALT_H