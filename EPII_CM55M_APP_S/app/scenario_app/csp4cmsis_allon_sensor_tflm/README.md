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
C++void Inference::run() {
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
C++void MainApp_Task(void* params) {
    static Channel<frame_t>  frame_chan;
    static Channel<result_t> result_chan;

    static Camera    camera(frame_chan.writer());
    static Inference inference(frame_chan.reader(), result_chan.writer());
    static Console   console(result_chan.reader());

    // Execute the parallel network
    Run(InParallel(camera, inference, console), ExecutionMode::StaticNetwork);
}
```

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
```

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

## 📝 License
This example is provided under the standard Himax SDK license terms. Refer to the top‑level license file in the SDK for details.
