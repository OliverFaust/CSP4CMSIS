#ifndef CSP_WRAPPER_H
#define CSP_WRAPPER_H

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <tuple>
#include <vector>
#include "csp4cmsis.h" 

// --- 1. START CSP NAMESPACE (For Definitions) ---
namespace csp {
    class CSProcess; // Defined in process.h
    
    enum class ExecutionMode {
        TerminatingNetwork, // Blocking: spawns all processes, waits for all to finish.
        StaticNetwork        // Non-blocking: spawns all processes, returns immediately.
    };
    
    // --- Internal Task Context DEFINITION ---
    struct TaskCtx {
        CSProcess* process;
        SemaphoreHandle_t completion_sem;
    };
} // end namespace csp definition block

// --- 2. The Globally Friended Task Wrapper (DECLARATION ONLY) ---
extern "C" {
    void ThreadFuncWrapper(void* pvParameters);
}


// --- 3. Continue CSP Namespace (For Template Logic) ---
namespace csp { 

// API 1.2: historical literal used by the pre-1.2 parallel spawner for
// every process except index 0. Preserved here as the fallback for any
// process that doesn't override stackWords() -- see process.h.
#ifndef CSP_LEGACY_PARALLEL_STACK_WORDS
#define CSP_LEGACY_PARALLEL_STACK_WORDS 256
#endif

// Historical composition-wide default priority (unchanged from pre-1.2).
#ifndef CSP_LEGACY_PARALLEL_PRIORITY
#define CSP_LEGACY_PARALLEL_PRIORITY (tskIDLE_PRIORITY + 2)
#endif

// --- Parallel Helper ---
template <typename... Processes>
class ParallelHelper {
private:
    std::tuple<Processes&...> procs;

    // API 1.2: spawns process I with ITS OWN declared stack/priority
    // (falling back to the historical literals if unspecified). Used
    // uniformly for every index, including 0 -- no process is special.
    template <std::size_t I>
    void spawn_task(SemaphoreHandle_t sem, UBaseType_t composition_priority) {
        TaskCtx* ctx = new TaskCtx{ &std::get<I>(procs), sem };

        size_t stack = resolveStackWords(std::get<I>(procs), CSP_LEGACY_PARALLEL_STACK_WORDS);
        UBaseType_t priority = resolveTaskPriority(std::get<I>(procs), composition_priority);

        xTaskCreate(
            (TaskFunction_t)ThreadFuncWrapper, 
            std::get<I>(procs).name(),
            stack, 
            ctx,
            priority,
            NULL
        );
    }

    // Spawns ALL processes, indices 0..N-1.
    template <std::size_t I>
    void spawn_all(SemaphoreHandle_t sem, UBaseType_t composition_priority) {
        if constexpr (I < sizeof...(Processes)) {
            spawn_task<I>(sem, composition_priority);
            spawn_all<I + 1>(sem, composition_priority);
        }
    }

public:
    explicit ParallelHelper(Processes&... p) : procs(p...) {}

    // 1. Blocking Run (ExecutionMode::TerminatingNetwork).
    // API 1.2: spawns ALL N processes (including index 0) as their own
    // tasks and blocks the CALLING task until all N have completed.
    // Previously, index 0 ran inline on the caller's stack; the caller
    // now does no CSP work of its own and can safely self-delete once
    // this returns, if it has nothing further to do.
    void execute_terminating(UBaseType_t composition_priority) {
        constexpr size_t num_procs = sizeof...(Processes);

        SemaphoreHandle_t done_sem = xSemaphoreCreateCounting(num_procs, 0);
        spawn_all<0>(done_sem, composition_priority);

        for (size_t i = 0; i < num_procs; ++i) {
            xSemaphoreTake(done_sem, portMAX_DELAY);
        }
        vSemaphoreDelete(done_sem);
    }

    // 2. Non-Blocking Run (ExecutionMode::StaticNetwork).
    // API 1.2: spawns ALL N processes (including index 0) and returns
    // immediately. No process runs on the calling task's stack.
    void execute_static(UBaseType_t composition_priority) {
        spawn_all<0>(NULL, composition_priority);
    }
};

// --- Public API Syntax ---

template <typename... Processes>
ParallelHelper<Processes...> InParallel(Processes&... procs) {
    return ParallelHelper<Processes...>(procs...);
}

// 1. Terminating-network Run(). 'priority' is the COMPOSITION-WIDE
// default: it applies to any process that hasn't overridden
// taskPriority(). The default value matches pre-1.2 behavior exactly.
template <typename... Processes>
void Run(ParallelHelper<Processes...> helper,
         UBaseType_t priority = CSP_LEGACY_PARALLEL_PRIORITY) {
    helper.execute_terminating(priority);
}

// 2. Explicit ExecutionMode selection. Same priority semantics as (1).
template <typename... Processes>
void Run(ParallelHelper<Processes...> helper, ExecutionMode mode,
         UBaseType_t priority = CSP_LEGACY_PARALLEL_PRIORITY) {
    if (mode == ExecutionMode::StaticNetwork) {
        helper.execute_static(priority);
    } else {
        helper.execute_terminating(priority);
    }
}

} // namespace csp

#endif // CSP_WRAPPER_H