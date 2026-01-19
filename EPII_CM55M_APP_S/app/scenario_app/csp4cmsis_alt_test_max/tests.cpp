#include "csp/csp4cmsis.h" 
#include <cstdio>
#include <vector>

#define NUM_SENDERS 16
#define TOTAL_MESSAGES_PER_SENDER 100000
#define MAX_TOTAL_MESSAGES (TOTAL_MESSAGES_PER_SENDER * NUM_SENDERS)
#define CHECK_INTERVAL 2000

using namespace csp;

struct Message {
    int source_id; 
    int sequence_num;
};

using AltChannel = Channel<Message>;

// --- Sender remains the same ---
class Sender : public CSProcess {
private:
    Chanout<Message> out;
    int id; 
public:
    Sender(Chanout<Message> w, int sender_id) : out(w), id(sender_id) {}
    const char* name() const override { 
        static char buf[12];
        snprintf(buf, sizeof(buf), "Snd%d", id);
        return buf; 
    }

    void run() override {
        for (int i = 0; i < TOTAL_MESSAGES_PER_SENDER; ++i) {
            out << Message{id, i}; 
        }
        while (true) vTaskDelay(portMAX_DELAY); 
    }
};

// --- Receiver Extended for 16 Channels ---
class Receiver : public CSProcess {
private:
    std::vector<Chanin<Message>> inputs;
public:
    Receiver(std::vector<Chanin<Message>> in_list) : inputs(in_list) {}
    const char* name() const override { return "Receiver"; }

    void run() override {
        printf("[Receiver] Stress Test: Monitoring %d Channels.\r\n", NUM_SENDERS);
        
        Message buffer[NUM_SENDERS];
        int next_seq[NUM_SENDERS] = {0};
        int total_count = 0;

        // Construct Alternative and add bindings manually
        Alternative alt;
        for(int i = 0; i < NUM_SENDERS; ++i) {
            alt.addBinding(inputs[i] | buffer[i]);
        }

        while(total_count < MAX_TOTAL_MESSAGES) {
            int selected = alt.fairSelect(); // Fair selection across 16 guards

            if (selected >= 0 && selected < NUM_SENDERS) {
                // Validate data integrity
                if (buffer[selected].sequence_num != next_seq[selected]) {
                    printf("!! ERROR Snd %d: Expected %d, Got %d\r\n", 
                            selected + 1, next_seq[selected], buffer[selected].sequence_num);
                }
                next_seq[selected]++;
                total_count++;
            }

            if (total_count % CHECK_INTERVAL == 0) {
                printf("[Receiver] Progress: %d / %d\r\n", total_count, MAX_TOTAL_MESSAGES);
            }
        }

        printf("[Receiver] SUCCESS: All %d messages verified.\r\n", total_count);
        while (true) vTaskDelay(portMAX_DELAY);
    }
};

// --- Main Application Task ---
void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(500)); 
    printf("--- Launching 16-Sender CSP Network ---\r\n");

    // 1. Static storage for channels
    static AltChannel channels[NUM_SENDERS];
    
    // 2. Static storage for Sender processes
    static Sender* senders[NUM_SENDERS];
    std::vector<Chanin<Message>> reader_list;

    for(int i = 0; i < NUM_SENDERS; ++i) {
        senders[i] = new Sender(channels[i].writer(), i + 1);
        reader_list.push_back(channels[i].reader());
    }

    // 3. Static storage for Receiver
    static Receiver r1(reader_list);

    // Note: To use InParallel with an array/vector, we would typically 
    // need a variadic expander. For this test, we can manually trigger 
    // the ParallelHelper or use a simpler loop to xTaskCreate if 
    // InParallel doesn't support runtime arrays.
    
    // Using the helper for the Receiver (index 0) and spawning senders
    auto helper = InParallel(r1, *senders[0], *senders[1], *senders[2], *senders[3],
                             *senders[4], *senders[5], *senders[6], *senders[7],
                             *senders[8], *senders[9], *senders[10], *senders[11],
                             *senders[12], *senders[13], *senders[14], *senders[15]);

    Run(helper, ExecutionMode::StaticNetwork); 
}

void RunProcessingChainTest(void) {
    // Note: Task creation is the only 'dynamic' part remaining, standard for FreeRTOS
    xTaskCreate(MainApp_Task, "MainApp", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
}
