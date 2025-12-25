#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

// --- 1. Basic Ring Components ---

class Buffer : public CSProcess {
    Chanin<int> in; Chanout<int> out;
public:
    Buffer(Chanin<int> r, Chanout<int> w) : in(r), out(w) {}
    void run() override {
        int x;
        while (true) { in >> x; out << x; }
    }
};

class Prefix : public CSProcess {
    Chanin<int> in; Chanout<int> out; int initial_val;
public:
    Prefix(Chanin<int> r, Chanout<int> w, int init) : in(r), out(w), initial_val(init) {}
    void run() override {
        out << initial_val; 
        int x;
        while (true) { in >> x; out << x; }
    }
};

class Successor : public CSProcess {
    Chanin<int> in; Chanout<int> out;
public:
    Successor(Chanin<int> r, Chanout<int> w) : in(r), out(w) {}
    void run() override {
        int x;
        while (true) { in >> x; out << (x + 1); }
    }
};

class Delta : public CSProcess {
    Chanin<int> in; Chanout<int> outA, outB;
public:
    Delta(Chanin<int> r, Chanout<int> wA, Chanout<int> wB) : in(r), outA(wA), outB(wB) {}
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

class ComstimeConsumer : public CSProcess {
    Chanin<int> data_in;
    Chanin<bool> trigger_in;
public:
    ComstimeConsumer(Chanin<int> data, Chanin<bool> trigger) 
        : data_in(data), trigger_in(trigger) {}

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

class Trigger : public CSProcess {
    Chanout<bool> out;
public:
    Trigger(Chanout<bool> w) : out(w) {}
    void run() override {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000)); 
            bool dummy = true;
            out << dummy;
        }
    }
};

// --- 4. Main App Task ---

void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Channels
    static Channel<int> c1, c2, c3, c4, cb1, cb2;
    static Channel<bool> c_trigger;

    // Process Instances
    static Successor proc_succ(c3.reader(), cb1.writer());
    static Buffer    proc_buf1(cb1.reader(), cb2.writer());
    static Buffer    proc_buf2(cb2.reader(),  c1.writer());
    static Prefix    proc_pref( c1.reader(),  c2.writer(), 0);
    static Delta     proc_delt( c2.reader(),  c3.writer(), c4.writer());
    static ComstimeConsumer proc_cons(c4.reader(), c_trigger.reader());
    static Trigger   proc_trig(c_trigger.writer());

    Run(
        InParallel(proc_succ, proc_buf1, proc_buf2, proc_pref, proc_delt, proc_cons, proc_trig),
        ExecutionMode::StaticNetwork
    );
}

extern "C" void RunProcessingChainTest(void) {
    xTaskCreate(MainApp_Task, "ComsMain", 2048, NULL, tskIDLE_PRIORITY + 3, NULL);
}
