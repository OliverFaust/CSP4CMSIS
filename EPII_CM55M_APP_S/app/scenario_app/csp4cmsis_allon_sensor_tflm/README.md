# AI Vision Application using CSP4CMSIS

## 📌 Overview

This project implements a complete AI inference pipeline on the Himax WE2 (Cortex‑M55) using the **CSP4CMSIS** library.  
It captures frames from an IMX219 sensor, runs a TensorFlow Lite Micro (TFLM) person‑detection model, and prints the prediction score to the UART console.

The application is structured as a network of lightweight CSP processes, providing clean decoupling, automatic back‑pressure, and maintainability.

---

## 🏗 Architecture

The pipeline is triggered by hardware interrupts from the sensor and flows through three processes connected by synchronous channels:

[IMX219 Sensor] --(IRQ)--> [g_trigger_chan] --trigger--> [Camera] --frame_t--> [Inference] --result_t--> [Console]


| Component | Responsibility |
|:---|:---|
| **`g_trigger_chan`** | A synchronous `Channel<trigger_t>` written from the sensor ISR (via `putFromISR()`) to signal a new frame. |
| **`Camera` process** | Initialises the sensor and data path. Waits on the trigger channel, retrieves JPEG info, and sends a `frame_t`. It then re‑triggers the hardware for the next frame. |
| **`frame_chan`** | Synchronous `Channel<frame_t>` connecting `Camera` → `Inference`. Carries JPEG buffer address and size. |
| **`Inference` process** | Initialises the TFLM model. Receives frames, runs inference, and outputs a `result_t` (frame index + prediction score). |
| **`result_chan`** | Synchronous `Channel<result_t>` connecting `Inference` → `Console`. |
| **`Console` process** | Prints the result to the console. |

All channels are **unbuffered** (`Channel<T>`), enforcing rendezvous synchronisation. This ensures that the pipeline automatically stalls if any stage is slower than its predecessor – a built‑in flow control.

---

## 🛠 Technical Details

### 1. Interrupt‑to‑CSP Bridge
The sensor driver calls `os_app_dplib_cb` from interrupt context. To safely handshake with the CSP network, the callback uses the ISR‑safe `putFromISR()` method:

```cpp
extern "C" void os_app_dplib_cb(SENSORDPLIB_STATUS_E event) {
    if (event == SENSORDPLIB_STATUS_XDMA_FRAME_READY) {
        g_trigger_chan.writer().putFromISR(trigger_t{});
    }
}

The camera process waits on the reader side, ensuring a lossless handshake.

### 2. Frame Re‑triggering
After sending a frame, the camera process explicitly calls sensordplib_retrigger_capture() to start capturing the next frame. This prevents buffer overrun and keeps the pipeline in lockstep.

# Model Inference
The inference process reuses the existing cvapp module from the Himax SDK:

Initialises the Ethos‑U55 NPU.

Loads the person‑detection model.

Runs inference on the raw YUV frame (the JPEG buffer is not used directly; the model reads the raw buffer via app_get_raw_addr()).

4. Memory & Performance
All channels are statically allocated – no heap usage inside CSP.

The entire network runs inside a single FreeRTOS task (cooperative multitasking).

Debug prints can be enabled/disabled by toggling DBG_MORE_INFO macros.

# 🔧 Key Code Snippets
camera_process.cpp (excerpt)
cpp
static Channel<trigger_t> g_trigger_chan;

extern "C" void os_app_dplib_cb(SENSORDPLIB_STATUS_E event) {
    if (event == SENSORDPLIB_STATUS_XDMA_FRAME_READY) {
        g_trigger_chan.writer().putFromISR(trigger_t{});
    }
}

void Camera::run() {
    auto trigger_reader = g_trigger_chan.reader();
    // ... initialisation ...

    while (true) {
        trigger_t t;
        trigger_reader.read(t);                     // wait for frame ready

        uint32_t jpeg_addr, jpeg_sz;
        cisdp_get_jpginfo(&jpeg_sz, &jpeg_addr);

        frame_t f = { m_frame_counter++, jpeg_addr, jpeg_sz };
        m_frame_out.write(f);                        // send to inference

        sensordplib_retrigger_capture();              // start next capture
    }
}
inference_process.cpp (excerpt)
cpp
void Inference::run() {
    if (cv_init(true, true) < 0) return;   // initialise model

    while (true) {
        frame_t f;
        m_frame_in.read(f);                  // wait for a frame

        // (optional cache invalidation)

        int8_t score = cv_run();              // run inference
        result_t res = { f.index, score };
        m_result_out.write(res);              // send to console
    }
}
console_process.cpp
cpp
void Console::run() {
    while (true) {
        result_t res;
        m_result_in.read(res);
        xprintf("Frame %lu: prediction = %d\n", res.frame_index, res.prediction);
    }
}
Network Construction (csp4cmsis_spn.cpp)
cpp
void MainApp_Task(void* params) {
    static Channel<frame_t>  frame_chan;
    static Channel<result_t> result_chan;

    static Camera    camera(frame_chan.writer());
    static Inference inference(frame_chan.reader(), result_chan.writer());
    static Console   console(result_chan.reader());

    Run(InParallel(camera, inference, console), ExecutionMode::StaticNetwork);
}

extern "C" void RunProcessingChainTest(void) {
    xTaskCreate(MainApp_Task, "CSP_Main", 8192, NULL, tskIDLE_PRIORITY + 3, NULL);
}

# 🚀 How to Run
Prerequisites
Hardware: Himax WE2‑based board (e.g., Seeed Grove Vision AI Module V2) with an IMX219 sensor.

Toolchain: ARM GNU toolchain (13.2.Rel1 or later).

Build system: Himax SDK Makefile environment.

Build Instructions
Navigate to the application directory:

bash
cd CSP4CMSIS/EPII_CM55M_APP_S
Set the application type:

bash
export APP_TYPE=csp4cmsis_allon_sensor_tflm
Clean and build:

bash
make clean
make
Flash the generated .elf file to the board (using J‑Link, X‑modem, or your preferred method).

Expected UART Output
text
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
Prediction values range from -128 to 127; higher values indicate person detected.

# 📁 File Structure
text
app/scenario_app/csp4cmsis_allon_sensor_tflm/
├── camera_process.h                // Camera process declaration
├── camera_process.cpp              // Camera implementation + ISR
├── inference_process.h             // Inference process declaration
├── inference_process.cpp           // Inference implementation
├── console_process.h               // Console process declaration
├── console_process.cpp             // Console implementation
├── common_types.h                  // Shared data types (frame_t, result_t, trigger_t)
├── csp4cmsis_spn.cpp               // CSP network construction & main task
└── csp4cmsis_allon_sensor_tflm.mk  // Build configuration (source list)

# 🐛 Troubleshooting
No "Frame ready IRQ" printed – check sensor power, connections, and that cisdp_sensor_start() is called successfully.

Crash after first IRQ – verify that putFromISR() is used, not write(). If you used a BufferedOne2OneChannel, switch to Channel.

Pipeline stalls after a few frames – ensure sensordplib_retrigger_capture() is called after each frame.

Linker errors (undefined symbols) – confirm all .cpp files are included in the build and that common_types.h defines the required structs.

# 📝 License
This example is provided under the standard Himax SDK license terms. Refer to the top‑level license file in the SDK for details.
