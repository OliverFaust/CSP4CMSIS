#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

struct Message {
    uint32_t val;
};

// --- 1. Define Channels ---
// Two buffered channels, each with 10 slots.
using NewestChan = BufferedOne2OneChannel<Message, 10, BufferPolicy::KeepNewest>;
using OldestChan = BufferedOne2OneChannel<Message, 10, BufferPolicy::KeepOldest>;

#define TEST_COUNT 1000000

// --- 2. Define Processes ---

class PolicySender : public CSProcess {
private:
    Chanout<Message> outNew;
    Chanout<Message> outOld;
public:
    PolicySender(Chanout<Message> n, Chanout<Message> o) : outNew(n), outOld(o) {}

    void run() override {
        printf("[Sender] Bursting %d messages to KeepNewest...\r\n", TEST_COUNT);
        for (uint32_t i = 0; i < TEST_COUNT; ++i) {
            outNew << Message{i};
        }

        printf("[Sender] Bursting %d messages to KeepOldest...\r\n", TEST_COUNT);
        for (uint32_t i = 0; i < TEST_COUNT; ++i) {
            outOld << Message{i};
        }

        printf("[Sender] Finished sending. Suspending.\r\n");
        vTaskSuspend(NULL);
    }
};

class PolicyReceiver : public CSProcess {
private:
    Chanin<Message> inNew;
    Chanin<Message> inOld;
public:
    PolicyReceiver(Chanin<Message> n, Chanin<Message> o) : inNew(n), inOld(o) {}

    void run() override {
        // Wait for sender to finish its non-blocking bursts
        vTaskDelay(pdMS_TO_TICKS(100));

        uint64_t sumNewest = 0;
        uint64_t sumOldest = 0;
        Message msg;

        printf("[Receiver] Draining KeepNewest buffer...\r\n");
        // We know there are 10 messages waiting in the buffer
        for (int i = 0; i < 10; ++i) {
            inNew >> msg;
            sumNewest += msg.val;
            printf("  Newest[%d]: %lu\r\n", i, (unsigned long)msg.val);
        }

        printf("[Receiver] Draining KeepOldest buffer...\r\n");
        for (int i = 0; i < 10; ++i) {
            inOld >> msg;
            sumOldest += msg.val;
            printf("  Oldest[%d]: %lu\r\n", i, (unsigned long)msg.val);
        }

        printf("\r\n--- FINAL RESULTS ---\r\n");
        printf("KeepNewest Total Accumulation: %llu\r\n", sumNewest);
        printf("KeepOldest Total Accumulation: %llu\r\n", sumOldest);
        
        if (sumNewest > sumOldest) {
            printf("HYPOTHESIS CONFIRMED: KeepNewest kept the high-sequence values.\r\n");
        }
        
        vTaskSuspend(NULL);
    }
};

// --- 3. Main Test Launcher ---

void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\r\n--- Launching Policy Comparison Test ---\r\n");

    static NewestChan chan_n;
    static OldestChan chan_o;

    static PolicySender  snd(chan_n.writer(), chan_o.writer());
    static PolicyReceiver rcv(chan_n.reader(), chan_o.reader());

    Run(
        InParallel(snd, rcv),
        ExecutionMode::StaticNetwork
    );
}

extern "C" void RunProcessingChainTest(void) {
    xTaskCreate(MainApp_Task, "PolicyTest", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
}
