#include "csp/csp4cmsis.h"
#include <cstdio>

extern "C" {
    #include "hx_drv_timer.h"
    #include "hx_drv_scu.h"
    #include "WE2_device.h"
    #include "WE2_core.h"
}

using namespace csp;

// Re-introducing the Channel
static Channel<uint32_t> timerChannel;

extern "C" void timer1_callback(uint32_t event) {
    hx_drv_timer_ClearIRQ(TIMER_ID_1);
    static uint32_t count = 0;
    count++;

    // ISR context: must use the ISR-safe, non-blocking write. The plain
    // write()/output() path can register the calling context as a waiting
    // task and block on a task notification -- not valid from an ISR, and
    // exactly what could cause the hang the old comment here warned about.
    timerChannel.writer().putFromISR(count);
}

class TimerProcess : public CSProcessStatic<256> {
public:
    const char* name() const override { return "TimerProcess"; }

    void run() override {
        printf("[CSP] Manual Engine Active. Waiting for Channel...\r\n");
        uint32_t val = 0;
        auto reader = timerChannel.reader();

        while (true) {
            // This blocks the task until the ISR writes to the channel
            reader.read(val); 
            printf(">>> CSP CHANNEL RECV: %lu\r\n", val);
        }
    }
};

class LogicProcess : public CSProcessStatic<256> {
public:
    const char* name() const override { return "LogicProcess"; }

    void run() override {
        while(true) {
            // Do some background AI or Logic
            vTaskDelay(pdMS_TO_TICKS(500));
            printf("Logic Heartbeat...\n");
        }
    }
};

void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("\r\n--- CSP4CMSIS Manual Channel Test ---\r\n");

    // Hardware Init
    hx_drv_scu_set_timer_clk_en(TIMER_ID_1, 1); 
    hx_drv_timer_init(TIMER_ID_1, HX_TIMER1_BASE);
    TIMER_CFG_T timer_cfg = {1000, TIMER_MODE_PERIODICAL, TIMER_CTRL_CPU, TIMER_STATE_DC};
    NVIC_EnableIRQ(TIMER1INT_IRQn);
    hx_drv_timer_hw_start(TIMER_ID_1, &timer_cfg, (Timer_ISREvent_t)timer1_callback);

    // Manual Execution: Just call the run method. 
    // The FreeRTOS task provides the 'life' for the process.
    static TimerProcess p1;
    static LogicProcess  p2;
    Run(InParallel(p1, p2), ExecutionMode::StaticNetwork);

    // Run() returns immediately in StaticNetwork mode; the task must
    // delete itself rather than fall off the end of the function.
    vTaskDelete(NULL);
}

extern "C" void RunProcessingChainTest(void) {
    xTaskCreate(MainApp_Task, "CspManual", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
}

