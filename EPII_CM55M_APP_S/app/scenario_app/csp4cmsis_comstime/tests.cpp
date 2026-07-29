#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

// How often to print a stack-usage report for the whole network. This is a
// *live* reading (CSProcess::stackHighWaterMarkWords()), not a one-shot
// end-of-run report -- every process in this ring runs forever by design,
// so the report loop below keeps running for the life of the task. Read
// the HWM columns to right-size the provisional CSProcessStatic<N> values
// below -- those numbers were picked before this test ever ran, not
// measured.
#ifndef CSP_STACK_REPORT_INTERVAL_MS
#define CSP_STACK_REPORT_INTERVAL_MS (3000)
#endif

// Set in RunProcessingChainTest() right after xTaskCreate() succeeds, so the
// report loop can also measure MainApp_Task's own stack headroom (it isn't
// a CSProcess, so it doesn't get a stackHighWaterMarkWords() of its own).
static TaskHandle_t s_main_app_task_handle = NULL;
#define MAIN_APP_STACK_WORDS 2048

// --- 1. Basic Ring Components ---

// CSProcess is abstract: stackWords()/stackBuffer()/taskBuffer() are pure
// virtual, so each process needs its own static stack + StaticTask_t
// storage. CSProcessStatic<N> is the library helper that supplies that
// storage -- inherit from it instead of CSProcess directly, with N as the
// stack size in words. 512 is a provisional starting point for the trivial
// ring stages (Prefix/Successor/Delta/Trigger); confirm/right-size against
// the HWM report added to MainApp_Task below.
class Prefix : public CSProcessStatic<512> {
    Chanin<int> in; Chanout<int> out; int initial_val;
public:
    Prefix(Chanin<int> r, Chanout<int> w, int init) : in(r), out(w), initial_val(init) {}
    const char* name() const override { return "Prefix"; }
    void run() override {
        out << initial_val; 
        int x;
        while (true) { in >> x; out << x; }
    }
};

class Successor : public CSProcessStatic<512> {
    Chanin<int> in; Chanout<int> out;
public:
    Successor(Chanin<int> r, Chanout<int> w) : in(r), out(w) {}
    const char* name() const override { return "Successor"; }
    void run() override {
        int x;
        while (true) { in >> x; out << (x + 1); }
    }
};

class Delta : public CSProcessStatic<512> {
    Chanin<int> in; Chanout<int> outA, outB;
public:
    Delta(Chanin<int> r, Chanout<int> wA, Chanout<int> wB) : in(r), outA(wA), outB(wB) {}
    const char* name() const override { return "Delta"; }
    void run() override {
        int x;
        while (true) {
            in >> x;
            outB << x; // Branch to Consumer
            outA << x; // Branch to Ring
        }
    }
};

// --- 2. The Consumer using ALT ---

// 1024 words is provisional: holds a 2-guard Alternative on the stack plus
// calls printf with float formatting (%.2f), which is stack-hungrier on
// newlib than plain integer printf. Confirm against the HWM report below.
class ComstimeConsumer : public CSProcessStatic<1024> {
    Chanin<int> data_in;
    Chanin<bool> trigger_in;
public:
    ComstimeConsumer(Chanin<int> data, Chanin<bool> trigger) 
        : data_in(data), trigger_in(trigger) {}
    const char* name() const override { return "ComstimeConsumer"; }

    void run() override {
        int val = 0;
        bool signal = false;
        uint32_t count = 0;
        const uint32_t benchmark_limit = 10000;

        Alternative alt(data_in | val, trigger_in | signal);

        printf("[Comstime] Benchmark starting. Measuring %lu cycles...\n", benchmark_limit);
        
        TickType_t start_time = xTaskGetTickCount();

        while (true) {
            int selected = alt.fairSelect();

            if (selected == 0) { 
                if (++count >= benchmark_limit) {
                    TickType_t end_time = xTaskGetTickCount();
                    float total_ms = (float)(end_time - start_time) * portTICK_PERIOD_MS;
                    float micro_per_loop = (total_ms * 1000.0f) / (float)benchmark_limit;
                    
                    printf("--- Comstime Results ---\r\n");
                    printf("Iterations: %lu\r\n", count);
                    printf("Total Time: %.2f ms\r\n", total_ms);
                    printf("Avg Latency: %.2f us/cycle\r\n", micro_per_loop);
                    printf("Last Value: %d\r\n", val);
                    printf("------------------------\r\n");
                    
                    count = 0;
                    start_time = xTaskGetTickCount();
                }
            } else if (selected == 1) {
                printf(">>> [ALT] External Trigger Event Latency Check <<<\r\n");
            }
        }
    }
};

// --- 3. External Trigger ---

class Trigger : public CSProcessStatic<512> {
    Chanout<bool> out;
public:
    Trigger(Chanout<bool> w) : out(w) {}
    const char* name() const override { return "Trigger"; }
    void run() override {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000)); 
            bool dummy = true;
            out << dummy;
        }
    }
};

// --- 4. Main App Task ---

void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Channels
    static Channel<int> c1, c2, c3, c4;
    static Channel<bool> c_trigger;

    // Process Instances
    static Successor proc_succ(c3.reader(), c1.writer());
    static Prefix    proc_pref( c1.reader(),  c2.writer(), 0);
    static Delta     proc_delt( c2.reader(),  c3.writer(), c4.writer());
    static ComstimeConsumer proc_cons(c4.reader(), c_trigger.reader());
    static Trigger   proc_trig(c_trigger.writer());

    auto network = InParallel(proc_succ, proc_pref, proc_delt, proc_cons, proc_trig);
    Run(network, ExecutionMode::StaticNetwork);

    printf("*** MainApp_Task: Run() returned, entering stack-report loop ***\r\n");

    // Every process in the ring runs forever by design, so there's no
    // "network finished" point to report stack usage at -- report
    // periodically instead.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CSP_STACK_REPORT_INTERVAL_MS));

        if (s_main_app_task_handle != NULL) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(s_main_app_task_handle);
            size_t unused_bytes = hwm * sizeof(StackType_t);
            printf("MainApp: %u bytes unused headroom (%u words HWM, of %u allocated)\r\n",
                    (unsigned)unused_bytes, (unsigned)hwm, (unsigned)MAIN_APP_STACK_WORDS);
        }

        network.forEachProcess([](CSProcess& p) {
            // API 1.3: stack depth is fixed at compile time -- no more
            // resolveStackWords()/fallback constant to consult, just ask
            // the process directly.
            size_t allocated_words = p.stackWords();
            size_t allocated_bytes = allocated_words * sizeof(StackType_t);

            UBaseType_t hwm = p.stackHighWaterMarkWords();
            if (hwm == CSP_STACK_HWM_UNAVAILABLE) {
                printf("%s: allocated = %u words (%u bytes), HWM unavailable\r\n",
                        p.name(), (unsigned)allocated_words, (unsigned)allocated_bytes);
            } else {
                size_t unused_bytes = hwm * sizeof(StackType_t);
                size_t used_bytes = (unused_bytes <= allocated_bytes)
                                        ? allocated_bytes - unused_bytes
                                        : 0; // guard against any inconsistency
                printf("%s: %u/%u bytes used (%u bytes unused headroom, %u words HWM)\r\n",
                        p.name(), (unsigned)used_bytes, (unsigned)allocated_bytes,
                        (unsigned)unused_bytes, (unsigned)hwm);
            }
        });
    }
}

extern "C" void RunProcessingChainTest(void) {
    BaseType_t status = xTaskCreate(MainApp_Task, "ComsMain", MAIN_APP_STACK_WORDS, NULL,
                                     tskIDLE_PRIORITY + 3, &s_main_app_task_handle);
    if (status != pdPASS) {
        printf("ERROR: MainApp_Task creation failed!\r\n");
    }
}
