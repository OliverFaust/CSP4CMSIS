# Test Scenario: 16-Sender ALT Stress Test (Max Capacity)

## 📌 Overview
The `csp4cmsis_alt_test_max` scenario is the ultimate stress test for the framework's **Alternative (ALT)** mechanism. It pushes the implementation to its architected limit by multiplexing **16 independent sender channels** into a single receiver process.

This test validates the stability of the **16-Guard Bit-Mapping** logic and the robustness of the **Fair Selection** algorithm under extreme congestion. It ensures that even with 1.6 million total messages ($16 \times 100,000$), the system maintains perfect data integrity and starvation-freedom.



---

## 🏗 Architecture

The process network is structured as a massive "Fan-In" pattern, utilizing the maximum guard capacity supported by the current bit-masking implementation:

| Component | Responsibility |
| :--- | :--- |
| **16 Senders** | Each sender operates in its own FreeRTOS task, attempting to saturate its specific channel with 100,000 sequenced messages. |
| **16 Channels** | Static `One2OneChannel` instances providing synchronous rendezvous points for each sender. |
| **1 Receiver** | Manages a single `Alternative` object with 16 registered bindings. It verifies the source and sequence of every incoming packet. |



---

## 🛠 Technical Details

### 1. Bit-Masking & Event Groups
The `Alternative` mechanism maps each of the 16 guards to a specific bit in a FreeRTOS Event Group. This test proves that:
* The mapping between `alt.addBinding()` index and Event Group bits is consistent.
* The system correctly handles "all-bits-set" scenarios where all 16 senders are waiting simultaneously.

### 2. Fair Selection at Scale
With 16 senders competing for the receiver's attention, a standard priority selection would lead to the starvation of higher-indexed senders. This test relies on `fairSelect()` to:
* Implement a circular (round-robin) search across the 16-bit mask.
* Guarantee an upper bound on latency for every sender in the network.



### 3. Memory & Performance
Despite the scale, the **Zero-Heap** philosophy remains intact. The `Alternative` metadata and guard pointers are managed on the stack, and the data transfer remains a direct copy between process contexts during the rendezvous.

---

## 🚀 How to Run

### Prerequisites
* **Hardware:** Himax WE2 (Cortex-M55). 
* **RTOS Configuration:** Ensure the FreeRTOS heap is sufficient to spawn 17 tasks (16 Senders + 1 Receiver), though the CSP primitives themselves remain static.
* **Make environment** in `CSP4CMSIS/EPII_CM55M_APP_S/makefile` set `APP_TYPE = csp4cmsis_alt_test_max`.

### Expected UART Output
The receiver will log progress every 2,000 messages. Total verification of 1,600,000 messages indicates a successful test.

```text
--- Launching 16-Sender CSP Network ---
[Receiver] Stress Test: Monitoring 16 Channels.
[Receiver] Progress: 2000 / 1600000
[Receiver] Progress: 4000 / 1600000
...
[Receiver] Progress: 1598000 / 1600000
[Receiver] SUCCESS: All 1600000 messages verified.
