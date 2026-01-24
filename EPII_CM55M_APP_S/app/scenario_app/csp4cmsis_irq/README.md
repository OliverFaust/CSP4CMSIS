# Test Scenario: Interrupt Service Routine (ISR) to Channel

## 📌 Overview
This test scenario demonstrates the **ISR-to-Process** communication pattern within the `csp4cmsis` framework. It serves as a verification of the framework's ability to safely bridge the gap between asynchronous hardware events and the synchronous Communicating Sequential Processes (CSP) world.

In this scenario, a hardware timer acts as a producer, injecting data into the CSP network. This is a foundational pattern for building event-driven embedded systems, such as sensor data acquisition or high-frequency control loops on the **Cortex-M55**.



---

## 🏗 Architecture

The test utilizes a `StaticNetwork` execution mode to run two concurrent processes alongside a hardware timer:

| Component | Context | Responsibility |
| :--- | :--- | :--- |
| **Timer 1 ISR** | Hardware IRQ | Fires every 1000ms; clears IRQ and writes an incrementing counter to the channel. |
| **TimerProcess** | CSP Process | Blocks on `timerChannel.read()`. Unblocks immediately when the ISR provides data. |
| **LogicProcess** | CSP Process | Simulates background AI logic or system monitoring; pulses a heartbeat every 500ms. |



---

## 🛠 Technical Details

### 1. Zero-Heap Synchronization
The `timerChannel` is declared as a static resource. This ensures that the communication path between the hardware interrupt and the consumer task is established at compile-time, adhering to the framework's **Zero-Heap** philosophy.

### 2. Context Transitioning
Communication follows a strict sequence to ensure thread safety:
1. The **ISR** captures hardware data and uses the `write()` (or `putFromISR`) method.
2. The **Framework** notifies the RTOS scheduler that a high-priority task is now ready.
3. The **TimerProcess** resumes execution, processes the data, and returns to a blocked state.

### 3. Non-Blocking Background Tasks
While the `TimerProcess` is waiting for hardware, the `LogicProcess` continues to execute. This demonstrates the framework's ability to manage multiple independent process lifetimes without interference or deadlocks.

---

## 🚀 How to Run

### Prerequisites
* **Hardware:** Himax WE2 (Cortex-M55) evaluation board.
* **Toolchain:** Arm GNU Toolchain (eabi).
* **Dependencies:** `hx_drv_timer` and `hx_drv_scu` libraries.
* **Make environment** in `CSP4CMSIS/EPII_CM55M_APP_S/makefile' set `APP_TYPE = csp4cmsis_irq'

### Expected UART Output
Upon successful execution, the UART console will display the background heartbeat interspersed with the data received from the hardware timer:

```text
--- CSP4CMSIS Manual Channel Test ---
[CSP] Manual Engine Active. Waiting for Channel...
Logic Heartbeat...
>>> CSP CHANNEL RECV: 1
Logic Heartbeat...
Logic Heartbeat...
>>> CSP CHANNEL RECV: 2
