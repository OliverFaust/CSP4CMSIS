# Test Scenario: Lossy Policy Validation (Newest vs. Oldest)
## 📌 Overview
The `csp4cmsis_lossy_policy_test` is a validation suite designed to demonstrate and verify Non-Blocking Buffer Policies. In real-time embedded systems, producers often generate data faster than consumers can process it (e.g., high-frequency sensor sampling).

This test proves that `csp4cmsis` can handle these "overrun" conditions gracefully by either preserving the original context (KeepOldest) or ensuring data freshness (KeepNewest), all while preventing the Producer from being throttled.

## 🏗 Architecture
The test creates a "Race Condition" by design, pairing a high-speed Burst Sender (the "Rabbit") with a Slow Receiver (the "Tortoise").

| Process | Responsibility |
| :--- | :--- |
| **PolicySender** | Rapidly injects 1,000,000 messages into two separate channels as fast as the CPU allows. |
| **PolicyReceiver** | Deliberately waits for the bursts to finish, then drains the buffers to inspect which data survived the "lossy" transition. |

## 🛠 Technical Details
### 1. The Hypothesis
   In a system where 1,000,000 items are pushed into a 10-slot buffer:
* KeepOldest should act as a "First-In-First-Out" gate that locks after the first 10 items.
* KeepNewest should act as a "Sliding Window" that always contains the most recent 10 items.
### 2. Implementation Logic
  The test uses the << operator for high-speed transmission. Because the channels are configured with non-blocking policies, the PolicySender does not perform a context switch to the receiver until the entire burst is completed or the time-slice expires.
### 3. Accumulation Verification
The receiver calculates the sum of the `seq_num` for the remaining 10 items in each buffer.
* KeepOldest Sum: $\sum_{i=0}^{9} i = 45$
* KeepNewest Sum: $\sum_{i=999,990}^{999,999} i = 9,999,945$

## 🚀 How to Run
### Prerequisites
* Hardware: Himax WE2 (Cortex-M55).
* Toolchain: Arm GNU Toolchain (GCC 13.2.1+).
* Build Config: In `EPII_CM55M_APP_S/makefile`, set:`APP_TYPE = csp4cmsis_lossy_policy_test`

### Expected UART Output
The test confirms the policy by comparing the final accumulation values. A success indicates that the internal circular buffer pointers are correctly wrapping and evicting data based on the chosen policy.

```Plaintext
--- Launching Policy Comparison Test -
[Sender] Bursting 1000000 messages to KeepNewest...
[Sender] Bursting 1000000 messages to KeepOldest...
[Sender] Finished sending. Suspending.
[Receiver] Draining KeepNewest buffer...
  Newest[0]: 999990
  ...
  Newest[9]: 999999
[Receiver] Draining KeepOldest buffer...
  Oldest[0]: 0
  ...
  Oldest[9]: 9

--- FINAL RESULTS ---
KeepNewest Total Accumulation: 9999945
KeepOldest Total Accumulation: 45
HYPOTHESIS CONFIRMED: KeepNewest kept the high-sequence values.
```
