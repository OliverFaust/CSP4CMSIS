// --- process.h (API 1.2) ---
#ifndef CSP4CMSIS_PROCESS_H
#define CSP4CMSIS_PROCESS_H

#include <stddef.h>   // For size_t, NULL definition
#include "FreeRTOS.h" // For UBaseType_t

extern "C" {
    void ThreadFuncWrapper(void* pvParameters);
}

// =============================================================
// API 1.2: Sentinel values for "this process expresses no opinion".
// Callers (Run() in public_task.h and run.h) fall back to the
// historical, path-specific default literal whenever a process
// returns these -- so a process that never overrides stackWords()/
// taskPriority() behaves exactly as it did before 1.2.
// =============================================================
#define CSP_STACK_UNSPECIFIED    ((size_t)0)
#define CSP_PRIORITY_UNSPECIFIED ((UBaseType_t)-1)

namespace csp {
    // Forward declarations of core internal classes
    namespace internal {
        class Kernel;
    }

    // =============================================================
    // CSP Public API Definition
    // =============================================================

    /**
     * @brief The abstract base class for all user-defined concurrent tasks (Processes).
     */
    class CSProcess {
    public:
        virtual ~CSProcess() = default;

        /**
         * @brief Returns the name of the process for FreeRTOS task registration.
         * Users can override this in their derived classes for better debugging.
         */
        virtual const char* name() const { return "csp_task"; }

        /**
         * @brief FreeRTOS task stack size, in words, required by this process
         * when spawned by Run() -- either as a single process or as part of
         * a parallel composition (InParallel(...)).
         *
         * API 1.2: previously, in a parallel composition, the process placed
         * FIRST in InParallel(...) ran inline on the *caller's* task/stack,
         * while every other process silently received a hardcoded 256-word
         * stack regardless of its actual needs. This made stack sizing
         * depend on argument order in a way that was invisible from the
         * public API and undocumented outside a source comment. As of 1.2,
         * every process -- in any position -- is spawned with the stack
         * size it declares here. The default (CSP_STACK_UNSPECIFIED) means
         * "use whatever this call path used before 1.2"; override only if
         * this specific process needs more.
         */
        virtual size_t stackWords() const { return CSP_STACK_UNSPECIFIED; }

        /**
         * @brief FreeRTOS task priority for this process when spawned by
         * Run(). If a composition-wide priority is also supplied to the
         * parallel Run(ParallelHelper<...>, ...) overloads, a priority
         * returned here (i.e. not CSP_PRIORITY_UNSPECIFIED) takes
         * precedence for this process only.
         */
        virtual UBaseType_t taskPriority() const { return CSP_PRIORITY_UNSPECIFIED; }

    protected:
        // C++CSP Standard: The primary process logic.
        virtual void run() = 0; 
        
        // C++CSP4CMSIS Extension: Called by the ThreadFuncWrapper upon completion.
        virtual void endProcess() {} 

    private:
        // The FreeRTOS wrapper function needs to access the protected run() method.
        friend void ::ThreadFuncWrapper(void* pvParameters);
    };

    // =============================================================
    // API 1.2: Shared resolution helpers.
    // Both Run() paths (single-process in public_task.h, parallel in
    // run.h) use these so the fallback logic lives in exactly one place.
    // =============================================================
    inline size_t resolveStackWords(const CSProcess& p, size_t fallback) {
        size_t sw = p.stackWords();
        return (sw != CSP_STACK_UNSPECIFIED) ? sw : fallback;
    }

    inline UBaseType_t resolveTaskPriority(const CSProcess& p, UBaseType_t fallback) {
        UBaseType_t pp = p.taskPriority();
        return (pp != CSP_PRIORITY_UNSPECIFIED) ? pp : fallback;
    }

} // namespace csp

namespace csp::internal {
    
    // Alias 'Process' to the new public name 'CSProcess'
    using Process = csp::CSProcess;

    // Type alias for convenience when passing process pointers
    using ProcessPtr = Process*;
    
    #define NullProcessPtr (static_cast<csp::internal::ProcessPtr>(NULL))

} // namespace csp::internal

#endif // CSP4CMSIS_PROCESS_H