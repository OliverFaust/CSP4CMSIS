# Test Scenario: Resident-Guard ALT (Zero-Heap Multiplexing)

## 📌 Overview
The `csp4cmsis_alt_test` scenario demonstrates the core **Alternative (ALT)** functionality of the framework. It focuses on the ability of a single receiver to multiplex between multiple input channels safely and deterministically without using dynamic memory allocation.

This test validates the **Resident-Guard** pattern, where synchronization guards live within the static memory of the channels themselves, and the selection logic is handled entirely on the task stack.



---

## 🏗 Architecture

The test consists of three concurrent processes that verify data integrity across non-deterministic selection:

| Process | Context | Responsibility |
| :--- | :--- | :--- |
| **Sender A** | Producer 1 | Sends 10,000 sequenced messages with ID `1` to `chan_A`. |
| **Sender B** | Producer 2 | Sends 10,000 sequenced messages with ID `2` to `chan_B`. |
| **Receiver** | Consumer | Uses a single `Alternative` object to wait on both channels and verifies sequence numbers. |



---

## 🛠 Technical Details

### 1. Resident-Guard Mechanism
In traditional CSP libraries, an `ALT` operation often dynamically creates a list of guards. In `csp4cmsis`, the `Alternative` object:
* **Borrows pointers** to guards already resident in the `Channel` objects.
* **Operates on the stack**, ensuring that once the `run()` method or the scope ends, all temporary selection metadata is cleared without needing a heap.

### 2. Fair Selection (fairSelect)
The receiver utilizes `fairSelect()`, which ensures that if both Sender A and Sender B are ready simultaneously, the framework rotates priority. This prevents one high-speed sender from "starving" the other, ensuring balanced throughput.

### 3. Data Integrity Verification
The test is designed to catch race conditions or synchronization errors. By checking both `source_id` and `sequence_num`, the test ensures that:
* Data is never interleaved or corrupted.
* Synchronous rendezvous is strictly enforced (the sender only proceeds once the receiver has picked that specific channel).

---

## 🚀 How to Run

### Prerequisites
* **Hardware:** Himax WE2 (Cortex-M55) or any ARMv8.1-M compatible board.
* **RTOS:** FreeRTOS configured with standard priority scheduling.
* * **Make environment** in `CSP4CMSIS/EPII_CM55M_APP_S/makefile` set `APP_TYPE = csp4cmsis_alt_test`.

### Expected UART Output
The console will log progress every 1,000 messages. Success is declared only when all 20,000 messages (10,000 per sender) are verified to be in the correct order.

```text
--- BOli2 Launching CSP Static Network (Zero-Heap) ---
[Sender 1] Starting sequence.
[Sender 2] Starting sequence.
[Receiver] Task running. Using Resident-Guard ALT.
[Receiver] Verified 1000 messages...
[Receiver] Verified 2000 messages...
...
[Receiver] SUCCESS: 20000 messages verified heap-free.
[Sender 1] Finished.
[Sender 2] Finished.
