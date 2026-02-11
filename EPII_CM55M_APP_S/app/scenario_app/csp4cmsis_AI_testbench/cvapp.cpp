#include "timer_utils.h"
#include "csp/csp4cmsis.h"
#include "sd_card_testbench.h"
#include "af_testbench_run.h"
#include <cstdio>
#include "WE2_debug.h"

using namespace csp;

struct work_packet_t {
    test_sample_t sample;
    uint32_t index;
};

struct result_packet_t {
    int8_t result;
    uint32_t index;
};

/**
 * @brief SD Process (Source)
 */
class SD : public CSProcess {
private:
    Chanout<work_packet_t> out;
    Chanin<result_packet_t> in;
public:
    SD(Chanout<work_packet_t> w, Chanin<result_packet_t> r) : out(w), in(r) {}
     
    void run() override {
        work_packet_t packet;
        result_packet_t result_packet;
        int max_index = 1*1024;
        uint32_t current_index = 0;    
        FRESULT fr;
        
        // Initialize timer (optional - can also call in app_main)
        timer_init();
        
        if (sd_card_init("blindfold_test_vectors","blindfold_test_vectors") != FR_OK) {
            dbg_printf(DBG_MORE_INFO, "SD card FatFs initialization failed in testbench_init!\r\n");
        }        
        dbg_printf(DBG_MORE_INFO, "[SD] Starting data fetch...\r\n");
        
        // Start timing the entire loop
        start_timer();
        
        while (current_index < max_index) {
            if (load_next_test_vector(current_index, &packet.sample, &packet.index) == FR_OK) {
                dbg_printf(DBG_MORE_INFO, "[SD] Sending index %lu...\r\n", packet.index);
                out << packet;
                in >> result_packet;
                fr = save_result_vector_bulk(result_packet.index, &result_packet.result, 1, "qdense_bulk_result_");
                if (fr != FR_OK) {
                    dbg_printf(DBG_MORE_INFO, "Failed to save results for sample %lu: %d\n", result_packet.index, fr);
                }                
                current_index = packet.index + 1;
            } else {
                stop_timer();  // This prints the timing results
                dbg_printf(DBG_MORE_INFO, "[SD] Exit");
                break; 
            }
        }
        
        // If loop completes normally, stop timer
        if (current_index >= max_index) {
            stop_timer();
            printf("[SD] All data queued. Processed %d samples.\r\n", max_index);
        }
        
        while (true) { vTaskDelay(portMAX_DELAY); }
    }
};

/**
 * @brief Inference Process (Transform)
 */
class InferenceEngine : public CSProcess {
private:
    Chanin<work_packet_t> in;
    Chanout<result_packet_t> out;
public:
    InferenceEngine(Chanin<work_packet_t> r, Chanout<result_packet_t> w) 
        : in(r), out(w) {}

    void run() override {
        work_packet_t packet;
        result_packet_t result_packet;
        
        dbg_printf(DBG_MORE_INFO, "[Inference] Initializing...\r\n");
        
        if(init_model(true, true) < 0) {
            dbg_printf(DBG_MORE_INFO, "Model init fail\r\n");
        } else {
            dbg_printf(DBG_MORE_INFO, "Model init success!\r\n");
        }

        while (true) {
            in >> packet; 
            dbg_printf(DBG_MORE_INFO, "[Inference] Processing index %lu\r\n", packet.index);  
            
            create_tensor(&packet.sample);
            run_model();
            get_model_result(&result_packet.result);
            
            result_packet.index = packet.index;
            out << result_packet; 
        }
    }
};

// --- 2. Network Construction ---
void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(500)); 

    printf("Pipeline starting...\r\n");
    
    /**
     * Pipeline Topology:
     * SD -> [c1] -> Inference -> [c2] -> Saver
     */
    static BufferedOne2OneChannel<work_packet_t, 16> c1;  // Buffer size of 16
    static BufferedOne2OneChannel<result_packet_t, 16> c2; // Buffer size of 16

    // Instantiate the processes as static to ensure they persist in the network
    static SD           proc_sd(c1.writer(),c2.reader());
    static InferenceEngine  proc_inference(c1.reader(), c2.writer());

    /**
     * SPN Execution: 
     * Compose the pipeline in Parallel.
     */
    Run(
        InParallel(
            proc_sd,
            proc_inference
        ),
        ExecutionMode::StaticNetwork
    );
}

extern "C" void RunProcessingChainTest(void) {
    xTaskCreate(MainApp_Task, "ComsMain", 8192, NULL, tskIDLE_PRIORITY + 3, NULL);
}
