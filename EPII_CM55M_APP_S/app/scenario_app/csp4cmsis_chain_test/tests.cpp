#include "csp/csp4cmsis.h"
#include <cstdio>
#include <vector>

using namespace csp;

// --- Configuration ---
#define NUM_RELAYS 5
#define TEST_ITERATIONS 1000
#define CHECK_INTERVAL 100

// --- 1. Define the Sequential Processes ---

/**
 * @brief Simple Source process.
 * Outputs an incrementing integer.
 */
class CountingSender : public CSProcess {
private:
    Chanout<int> out;
public:
    CountingSender(Chanout<int> w) : out(w) {}

    void run() override {
        printf("[Sender] Starting stream...\r\n");
        for (int i = 1; i <= TEST_ITERATIONS; ++i) {
            out << i;
        }
        printf("[Sender] Stream complete.\r\n");
    }
};

/**
 * @brief The Relay process.
 * Complies with SPN by being a purely sequential actor:
 * Inputs from one channel, outputs to another.
 */
class Relay : public CSProcess {
private:
    Chanin<int> in;
    Chanout<int> out;
    int id;
public:
    Relay(Chanin<int> r, Chanout<int> w, int relay_id) 
        : in(r), out(w), id(relay_id) {}

    void run() override {
        int data;
        for (int i = 0; i < TEST_ITERATIONS; ++i) {
            in >> data;
            out << data;
        }
    }
};

/**
 * @brief Sink process.
 * Verifies the data integrity across the network.
 */
class CheckerReceiver : public CSProcess {
private:
    Chanin<int> in;
public:
    CheckerReceiver(Chanin<int> r) : in(r) {}

    void run() override {
        int received;
        bool success = true;
        for (int i = 1; i <= TEST_ITERATIONS; ++i) {
            in >> received;
            if (received != i) {
                printf("[Receiver] !! DATA ERROR: Expected %d, Got %d\r\n", i, received);
                success = false;
                break;
            }
            if (i % CHECK_INTERVAL == 0) {
                printf("[Receiver] Verified up to %d...\r\n", i);
            }
        }
        if (success) {
            printf("[Receiver] SUCCESS: All %d values verified through %d relays.\r\n", 
                   TEST_ITERATIONS, NUM_RELAYS);
        }
    }
};

// --- 2. Network Construction ---

void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(50)); 
    printf("\r\n--- Launching CSP Relay Chain (SPN Principle) ---\r\n");

    /**
     * To connect N relays, we need N+1 channels:
     * Sender -> [C0] -> Relay1 -> [C1] -> Relay2 -> [C2] -> Receiver
     */
    static Channel<int> channels[NUM_RELAYS + 1];

    // Instantiate the Sender and Receiver at the chain ends
    static CountingSender sender(channels[0].writer());
    static CheckerReceiver receiver(channels[NUM_RELAYS].reader());

    // Instantiate the Relays to bridge the channels
    static Relay relays[NUM_RELAYS] = {
        Relay(channels[0].reader(), channels[1].writer(), 0),
        Relay(channels[1].reader(), channels[2].writer(), 1),
        Relay(channels[2].reader(), channels[3].writer(), 2),
        Relay(channels[3].reader(), channels[4].writer(), 3),
        Relay(channels[4].reader(), channels[5].writer(), 4)
    };

    /**
     * SPN Execution: 
     * We compose all processes in Parallel.
     * The rendezvous channels will handle the synchronization.
     */
    Run(
        InParallel(
            sender,
            relays[0],
            relays[1],
            relays[2],
            relays[3],
            relays[4],
            receiver
        ),
        ExecutionMode::StaticNetwork
    );
}

void RunProcessingChainTest(void) {
    // Note: Task creation is the only 'dynamic' part remaining, standard for FreeRTOS
    xTaskCreate(MainApp_Task, "MainApp", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
}
