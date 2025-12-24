#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

// --- 1. The Number Generator ---
// Sends numbers 2, 3, 4, 5... into the chain.
class NaturalNumbers : public CSProcess {
private:
    Chanout<int> out;
    int limit;
public:
    NaturalNumbers(Chanout<int> w, int max_n) : out(w), limit(max_n) {}

    void run() override {
        for (int i = 2; i <= limit; ++i) {
            out << i;
        }
        // Signal end of stream if needed, or just park
        while(true) vTaskDelay(portMAX_DELAY);
    }
};

// --- 2. The Prime Filter Process ---
// Holds a prime 'p'. For every 'n' it receives, it only passes 'n' 
// to the next stage if (n % p != 0).
class PrimeFilter : public CSProcess {
private:
    Chanin<int> in;
    Chanout<int> out;
    int my_prime = -1;
    int id;
public:
    PrimeFilter(Chanin<int> r, Chanout<int> w, int filter_id) 
        : in(r), out(w), id(filter_id) {}

    void run() override {
        int candidate;
        
        // The very first number this filter receives is its own prime
        in >> my_prime;
        printf("[Filter %d] Discovered Prime: %d\r\n", id, my_prime);

        while (true) {
            in >> candidate;
            if (candidate % my_prime != 0) {
                // Not divisible, pass it to the next filter in the chain
                out << candidate;
            }
        }
    }
};

// --- 3. The Sink (Receiver) ---
// The final stage that just prints whatever makes it through all filters.
class PrimeSink : public CSProcess {
private:
    Chanin<int> in;
public:
    PrimeSink(Chanin<int> r) : in(r) {}
    void run() override {
        int found;
        while (true) {
            in >> found;
            printf("[Sink] Leaked through all filters: %d (Potential Prime)\r\n", found);
        }
    }
};

// --- 4. Main Network Construction ---
#define NUM_FILTERS 5 

void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\r\n--- Launching Prime Sieve Daisy Chain ---\r\n");

    // We need NUM_FILTERS + 1 channels to connect the stages
    // Generator -> [C0] -> Filter0 -> [C1] -> Filter1 -> [C2] -> Sink
    static Channel<int> channels[NUM_FILTERS + 1];

    static NaturalNumbers generator(channels[0].writer(), 50);
    static PrimeSink sink(channels[NUM_FILTERS].reader());

    // Create the filters
    static PrimeFilter f0(channels[0].reader(), channels[1].writer(), 0);
    static PrimeFilter f1(channels[1].reader(), channels[2].writer(), 1);
    static PrimeFilter f2(channels[2].reader(), channels[3].writer(), 2);
    static PrimeFilter f3(channels[3].reader(), channels[4].writer(), 3);
    static PrimeFilter f4(channels[4].reader(), channels[5].writer(), 4);

    Run(
        InParallel(generator, f0, f1, f2, f3, f4, sink),
        ExecutionMode::StaticNetwork
    );
}

void RunProcessingChainTest(void) {
    // Note: Task creation is the only 'dynamic' part remaining, standard for FreeRTOS
    xTaskCreate(MainApp_Task, "MainApp", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
}
