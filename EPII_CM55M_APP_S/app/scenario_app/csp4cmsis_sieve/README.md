# Test Scenario: Sieve of Eratosthenes (Daisy Chain)

## 📌 Overview
The `csp4cmsis_sieve` scenario implements a concurrent version of the **Sieve of Eratosthenes**. It demonstrates a dynamic-logic pipeline where data is filtered through a "Daisy Chain" of processes. Each stage in the chain is responsible for a single prime number and filters out all multiples of that prime from the stream.

This test validates the framework's ability to handle **functional pipelining**—a common pattern in signal processing and data stream analysis where different processes perform sequential transformations on a continuous data flow.



---

## 🏗 Architecture

The network is structured as a linear pipeline. Each `PrimeFilter` process "claims" the first number it receives as a prime and then acts as a modulo-filter for all subsequent numbers.

| Component | Responsibility |
| :--- | :--- |
| **NaturalNumbers** | The "Generator". Pumps a sequence of integers (2, 3, 4...) into the head of the pipeline. |
| **PrimeFilter (x5)** | The "Worker". Captures its first input as `my_prime` and drops any subsequent inputs divisible by it. |
| **PrimeSink** | The "Final Sink". Receives and prints any number that was not divisible by any of the primes held by the preceding filters. |

### The Pipeline Flow
`Generator` → `[C0]` → `Filter(2)` → `[C1]` → `Filter(3)` → `[C2]` → `Filter(5)` → `[C3]` → `Filter(7)` → `[C4]` → `Filter(11)` → `[C5]` → `Sink`

---

## 🛠 Technical Details

### 1. Zero-Capacity Synchronization
Like the Relay Chain, this Sieve relies on **Synchronous Rendezvous**. This means the `NaturalNumbers` generator cannot flood the network; it can only produce a new number when the first filter is ready to receive. This creates a self-throttled system that perfectly matches the processing speed of the filters.



### 2. State-Based Processing
Each `PrimeFilter` maintains an internal state. The first `read` operation populates the prime identity of that specific process. Subsequent `read` and `write` operations form the filtering logic. This demonstrates how CSP processes can encapsulate local state safely without needing global variables or external locks.

### 3. Static Network Orchestration
Even though the logic represents a "growing" filter chain, the resources are allocated using `ExecutionMode::StaticNetwork`. On the **Cortex-M55**, this provides the deterministic memory footprint required for embedded safety, even when running complex algorithmic pipelines.

---

## 🚀 How to Run

### Prerequisites
* **Hardware:** Himax WE2 (Cortex-M55).
* **Environment:** FreeRTOS environment.
* **Make environment** in `CSP4CMSIS/EPII_CM55M_APP_S/makefile` set `APP_TYPE = csp4cmsis_sieve`.

### Expected UART Output
The console will show each filter discovering its prime, followed by the "Sink" catching values that survived the entire chain (e.g., if you have 5 filters for 2, 3, 5, 7, and 11, the Sink will start catching 13, 17, 19...).

```text
--- Launching Prime Sieve Daisy Chain ---
[Filter 0] Discovered Prime: 2
[Filter 1] Discovered Prime: 3
[Filter 2] Discovered Prime: 5
[Filter 3] Discovered Prime: 7
[Filter 4] Discovered Prime: 11
[Sink] Leaked through all filters: 13 (Potential Prime)
[Sink] Leaked through all filters: 17 (Potential Prime)
