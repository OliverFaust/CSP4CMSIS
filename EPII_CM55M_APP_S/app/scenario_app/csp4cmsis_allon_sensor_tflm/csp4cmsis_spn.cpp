#include "csp/csp4cmsis.h"
#include "camera_process.h"
#include "inference_process.h"
#include "console_process.h"
#include "xprintf.h"

using namespace csp;

void MainApp_Task(void* params)
{
    vTaskDelay(pdMS_TO_TICKS(500));

    // Create channels
    static Channel<frame_t>  frame_chan;      // unbuffered – can be buffered if needed
    static Channel<result_t> result_chan;

    // Instantiate processes
    static Camera camera(frame_chan.writer());
    static Inference inference(frame_chan.reader(), result_chan.writer());
    static Console   console(result_chan.reader());

    // Run the CSP network
    Run(InParallel(camera, inference, console), ExecutionMode::StaticNetwork);
}

extern "C" void RunProcessingChainTest(void)
{
    xTaskCreate(MainApp_Task, "CSP_Main", 8192, NULL, tskIDLE_PRIORITY + 3, NULL);
}

