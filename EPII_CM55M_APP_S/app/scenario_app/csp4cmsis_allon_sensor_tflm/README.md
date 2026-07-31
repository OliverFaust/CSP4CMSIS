# CSP-based Person Detection on Himax WE2

This project implements a robust, lossless image processing pipeline using **Communicating Sequential Processes (CSP)** patterns on the Himax WE2 (Grove Vision AI V2) hardware. It leverages FreeRTOS and the Ethos-U55 NPU to perform real-time person detection.



## 🏗 Architecture Overview

The system is designed as a **Static Network** of independent processes communicating via synchronous, zero-copy channels.

### 1. Camera Handshake (ISR to Task)
To ensure no frames are dropped or partially read, the system uses a private trigger channel:
* **The ISR:** `os_app_dplib_cb` signals the arrival of a new frame using the non-blocking `putFromISR()`.
* **The Process:** The Camera process blocks on this trigger, ensuring a lossless, synchronized handshake between hardware and software.

### 2. Frame Re-triggering
To prevent buffer overruns and keep the pipeline in lockstep, the Camera process explicitly calls `sensordplib_retrigger_capture()` only **after** the current frame has been successfully passed to the inference stage. This creates natural back-pressure.

### 3. Model Inference
The inference process utilizes the `cvapp` module from the Himax SDK:
* **NPU:** Initialises the Ethos‑U55.
* **Model:** Loads a specialized person‑detection TFLM model.
* **Data Path:** Runs inference on raw YUV frames (accessed via `app_get_raw_addr()`).

### 4. Memory & Performance
* **Zero Heap:** All CSP channels and processes are statically allocated at compile time.
* **Concurrency:** The entire network runs inside a single high-priority FreeRTOS task using the CSP cooperative scheduler.

---

## 🔧 Key Code Snippets

### Camera Process (`camera_process.cpp`)
```cpp
static Channel<trigger_t> g_trigger_chan;

extern "C" void os_app_dplib_cb(SENSORDPLIB_STATUS_E event) {
    if (event == SENSORDPLIB_STATUS_XDMA_FRAME_READY) {
        // Signal the task from the ISR context safely
        g_trigger_chan.writer().putFromISR(trigger_t{});
    }
}

void Camera::run() {
    auto trigger_reader = g_trigger_chan.reader();
    // ... sensor initialisation ...

    while (true) {
        trigger_t t;
        trigger_reader.read(t);                      // Block until ISR fires

        uint32_t jpeg_addr, jpeg_sz;
        cisdp_get_jpginfo(&jpeg_sz, &jpeg_addr);

        frame_t f = { m_frame_counter++, jpeg_addr, jpeg_sz };
        m_frame_out.write(f);                        // Send to inference (blocks if busy)

        sensordplib_retrigger_capture();             // Acknowledge and allow next capture
    }
}
```

### Inference Process (`inference_process.cpp`)
```cpp
void Inference::run() {
    if (cv_init(true, true) < 0) return;             // Initialise NPU

    while (true) {
        frame_t f;
        m_frame_in.read(f);                          // Wait for frame from Camera

        int8_t score = cv_run();                     // Run NPU inference
        
        result_t res = { f.index, score };
        m_result_out.write(res);                     // Send result to Console
    }
}
```
### Network Construction (`csp4cmsis_spn.cpp`)
```cpp
void MainApp_Task(void* params) {
    static Channel<frame_t>  frame_chan;
    static Channel<result_t> result_chan;

    static Camera    camera(frame_chan.writer());
    static Inference inference(frame_chan.reader(), result_chan.writer());
    static Console   console(result_chan.reader());

    // Kept as a named object -- ParallelHelper still holds references to
    // camera/inference/console after Run() returns, which the stack
    // reporting loop below needs.
    auto network = InParallel(camera, inference, console);
    Run(network, ExecutionMode::StaticNetwork);

    // camera/inference/console each run forever, so there's no natural
    // "network finished" point -- instead, MainApp_Task periodically
    // walks the network and reports FreeRTOS stack high-water-mark
    // usage for every process, plus its own.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CSP_STACK_REPORT_INTERVAL_MS));
        // ... report CSP_Main HWM, then network.forEachProcess(...) ...
    }
}
```

## 📊 Stack Occupancy Reporting

`MainApp_Task` isn't a `CSProcess` itself, so it isn't sized via `CSProcessStatic<N>` -- its stack is allocated explicitly (`MAIN_APP_STACK_WORDS`, currently 512 words) and given to `xTaskCreateStatic` along with a static TCB, the same way FreeRTOS supplies its idle/timer task hooks.

Once the network is started, `MainApp_Task` becomes a monitoring loop: every `CSP_STACK_REPORT_INTERVAL_MS` (default 3000 ms, overridable at compile time) it:

* Reads its own high-water-mark via `uxTaskGetStackHighWaterMark()` and reports `CSP_Main`'s unused headroom.
* Calls `network.forEachProcess(...)` to walk every process in the static network and report each one's allocated stack, used bytes, unused headroom, and HWM in words. If a process doesn't expose a HWM, this is reported as unavailable rather than guessed at.

Because camera/inference/console all run forever, there's no natural "network finished" point to take a single reading at -- these are *live, worst-observed-so-far* readings, and are only trustworthy once each process's deepest call path has actually been exercised (e.g. after error/edge-case branches in inference or console have run at least once).

### Sample Output
```text
invoke pass
person_score:-37
Frame 55: prediction = -37
Frame 55: prediction = -37
CSP_Main: 1760 bytes unused headroom (440 words HWM, of 512 allocated)
Camera: 744/1024 bytes used (280 bytes unused headroom, 70 words HWM)
Inference: 472/1024 bytes used (552 bytes unused headroom, 138 words HWM)
Console: 256/1024 bytes used (768 bytes unused headroom, 192 words HWM)
Camera: retrigger hardware for next frame
```

| Task | Allocated | Used | Unused Headroom | HWM (words) |
|---|---|---|---|---|
| CSP_Main | 2048 bytes (512 words) | -- | 1760 bytes | 440 |
| Camera | 1024 bytes | 744 bytes | 280 bytes | 70 |
| Inference | 1024 bytes | 472 bytes | 552 bytes | 138 |
| Console | 1024 bytes | 256 bytes | 768 bytes | 192 |

Camera is the process closest to its allocation (744/1024 bytes used, only 280 bytes of headroom) -- worth keeping an eye on if the sensor-handling path grows. Inference and Console both have more comfortable margins, at under half and a quarter of their 1024-byte stacks respectively, despite Inference being the process that drives the Ethos‑U55 NPU call. `CSP_Main` itself has over 1.7 KB of its 2 KB allocation free even after taking on the monitoring loop.

## 🚀 How to Run
### Prerequisites

* **Hardware:** Himax WE2‑based board (Grove Vision AI Module V2).
* **Sensor:** IMX219 Camera Module.
* **Toolchain:** ARM GNU Toolchain (13.2.Rel1 or later).
* **Build system:** Himax WE2 SDK.

### Build Instructions
1. Navigate to the application directory:
```
CSP4CMSIS/EPII_CM55M_APP_S
```
2. Set the application type in the `makefile`:
```
APP_TYPE=csp4cmsis_allon_sensor_tflm
```
3. Build the project:
```
make clean 
make
```
4. Flash the generated `.elf` file using J-Link or your preferred programmer.

### Expected UART Output
```text
Camera: initializing sensor
Ethos-U55 device initialised
model's schema version 3
initial done
Frame ready IRQ
Trigger send
Camera: retrigger hardware for next frame
Frame 0: prediction = -128
Frame 1: prediction = -127
...
*** MainApp_Task: Run() returned, entering report loop ***
...
CSP_Main: 1760 bytes unused headroom (440 words HWM, of 512 allocated)
Camera: 744/1024 bytes used (280 bytes unused headroom, 70 words HWM)
Inference: 472/1024 bytes used (552 bytes unused headroom, 138 words HWM)
Console: 256/1024 bytes used (768 bytes unused headroom, 192 words HWM)
...
```
Stack occupancy lines like the block above are printed every `CSP_STACK_REPORT_INTERVAL_MS` (3 s by default) once the network is running -- see [Stack Occupancy Reporting](#-stack-occupancy-reporting) below.

## 📁 File Structure
```text
app/scenario_app/csp4cmsis_allon_sensor_tflm/
├── camera_process.h        // Camera process declaration
├── camera_process.cpp      // Camera implementation + ISR handling
├── inference_process.h     // Inference process declaration
├── inference_process.cpp   // Inference implementation (Ethos-U55)
├── console_process.h       // Console process declaration
├── console_process.cpp     // Console implementation (UART output)
├── common_types.h          // Shared structs (frame_t, result_t)
├── csp4cmsis_spn.cpp       // Network construction & Main task
└── app.mk                  // Makefile source list
```

## 🐛 Troubleshooting
* No "Frame ready IRQ"Hardware InitVerify cisdp_sensor_start() returns 0 and check sensor cables.
* Crash after first IRQISR BlockingEnsure you use putFromISR(), not write() inside the callback.
* Pipeline stallsMissing RetriggerEnsure sensordplib_retrigger_capture() is called at the end of the camera loop.
* Memory OverrunBuffer sizeIf using BufferedChannel, ensure the size is sufficient for your FPS.
* Stack overflow / corruptionTask stack too smallCheck the periodic stack report (see [Stack Occupancy Reporting](#-stack-occupancy-reporting)); if a process's unused headroom is trending toward 0, increase its `CSProcessStatic<N>` size (or `MAIN_APP_STACK_WORDS` for `CSP_Main`).

## 📝 License
This example is provided under the standard Himax SDK license terms. Refer to the top‑level license file in the SDK for details.
