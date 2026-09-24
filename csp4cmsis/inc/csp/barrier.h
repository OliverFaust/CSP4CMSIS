// --- barrier.h ---
#ifndef CSP4CMSIS_BARRIER_H
#define CSP4CMSIS_BARRIER_H

#include "cmsis_os2.h"
#include <stddef.h> // For size_t

namespace csp {

    namespace internal {

        /**
         * @brief A reusable synchronization point where a fixed number of processes
         * must arrive before any are allowed to proceed.
         */
        class Barrier {
        private:
            const size_t max_processes;
            size_t count; // Protected by count_mutex

            osMutexId_t count_mutex;    // Protects the 'count' variable
            osSemaphoreId_t wait_sem;   // Used to block and release tasks

        public:
            /**
             * @brief Constructs a barrier that requires N processes to synchronize.
             * @param N The required number of processes.
             */
            Barrier(size_t N);

            /**
             * @brief Cleans up the CMSIS-RTOS2 synchronization objects.
             */
            ~Barrier();

            /**
             * @brief Blocks the calling task until all N processes have arrived.
             */
            void sync();
        };

    } // namespace csp::internal

    // Alias in the main csp namespace for user-friendliness
    using Barrier = internal::Barrier;

} // namespace csp

#endif // CSP4CMSIS_BARRIER_H
