#include "csp/csp4cmsis.h"
#include "camera_process.h"
#include "inference_process.h"
#include "console_process.h"
#include "xprintf.h"

using namespace csp;

// camera/inference/console run forever, so there's no natural
// "network finished" point to report stack usage at -- report
// periodically instead. This is a live worst-observed-so-far reading,
// trustworthy only once each process's deepest call path has actually
// been exercised.
#ifndef CSP_STACK_REPORT_INTERVAL_MS
#define CSP_STACK_REPORT_INTERVAL_MS (3000)
#endif

// MainApp_Task isn't a CSProcess, so its stack isn't sized via
// CSProcessStatic<N> -- it's given explicitly here.
#define MAIN_APP_STACK_WORDS 512

static TaskHandle_t s_main_app_task_handle = NULL;

void MainApp_Task(void* params)
{
    vTaskDelay(pdMS_TO_TICKS(500));

    static Channel<frame_t>  frame_chan;      // unbuffered
    static Channel<result_t> result_chan;

    static Camera camera(frame_chan.writer());
    static Inference inference(frame_chan.reader(), result_chan.writer());
    static Console   console(result_chan.reader());

    xprintf("BOli\r\n");
    // Kept as a named object rather than passed straight into Run() --
    // ParallelHelper still holds references to camera/inference/console
    // after Run() returns, which the report loop below needs.
    auto network = InParallel(camera, inference, console);
    Run(network, ExecutionMode::StaticNetwork);

    xprintf("*** MainApp_Task: Run() returned, entering report loop ***\r\n");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CSP_STACK_REPORT_INTERVAL_MS));

        if (s_main_app_task_handle != NULL) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(s_main_app_task_handle);
            size_t unused_bytes = hwm * sizeof(StackType_t);
            xprintf("CSP_Main: %u bytes unused headroom (%u words HWM, of %u allocated)\r\n",
                    (unsigned)unused_bytes, (unsigned)hwm, (unsigned)MAIN_APP_STACK_WORDS);
        }

        network.forEachProcess([](CSProcess& p) {
            size_t allocated_words = p.stackWords();
            size_t allocated_bytes = allocated_words * sizeof(StackType_t);

            UBaseType_t hwm = p.stackHighWaterMarkWords();
            if (hwm == CSP_STACK_HWM_UNAVAILABLE) {
                xprintf("%s: allocated = %u words (%u bytes), HWM unavailable\r\n",
                        p.name(), (unsigned)allocated_words, (unsigned)allocated_bytes);
            } else {
                size_t unused_bytes = hwm * sizeof(StackType_t);
                size_t used_bytes = (unused_bytes <= allocated_bytes)
                                        ? allocated_bytes - unused_bytes
                                        : 0;
                xprintf("%s: %u/%u bytes used (%u bytes unused headroom, %u words HWM)\r\n",
                        p.name(), (unsigned)used_bytes, (unsigned)allocated_bytes,
                        (unsigned)unused_bytes, (unsigned)hwm);
            }
        });
    }
}

// MainApp_Task isn't a CSProcess, so its stack and TCB are supplied
// directly, the same way FreeRTOS's idle/timer task hooks do.
static StackType_t s_main_app_stack[MAIN_APP_STACK_WORDS];
static StaticTask_t s_main_app_tcb;

extern "C" void RunProcessingChainTest(void)
{
    TaskHandle_t handle = xTaskCreateStatic(
        MainApp_Task,
        "CSP_Main",
        MAIN_APP_STACK_WORDS,
        NULL,
        tskIDLE_PRIORITY + 3,
        s_main_app_stack,
        &s_main_app_tcb
    );

    s_main_app_task_handle = handle; // NULL on failure -- report loop guards for that

    if (handle == NULL) {
        xprintf("FATAL ERROR: Failed to create MainApp_Task "
                "(xTaskCreateStatic returned NULL -- check stack/TCB buffers).\r\n");
    }
}
