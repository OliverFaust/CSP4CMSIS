# Scenario: Interrupt-Driven Gyroscope Acquisition (L3G4200D)

## 📌 Overview
The `csp4cmsis_shake_detection` (or sensor acquisition) scenario demonstrates how to integrate hardware interrupts with the **CSP4CMSIS** framework. It interfaces an STM32 NUCLEO-F401RE with an L3G4200D gyroscope using SPI and an EXTI (External Interrupt) line.

Instead of inefficiently polling the sensor, this architecture relies on the L3G4200D's hardware `DRDY` (Data Ready) pin to trigger a FreeRTOS task only when fresh data is available. This data is then seamlessly routed through a synchronous CSP channel for downstream processing.

---

## 🏗 Architecture & Process Network

The system isolates hardware interaction from application logic using a producer-consumer CSP network.

```text
  [Hardware]                        [RTOS / CSP Context]

+------------+      EXTI (PA1)      +-----------------+
|            | -------------------> |    EXTI1_IRQn   |
|  L3G4200D  |                      +-----------------+
|  Gyroscope |                                | (Binary Semaphore)
|            |      SPI3 Read       +-----------------+
|            | <------------------- |  SensorReader   | (Producer Process)
+------------+                      +-----------------+
                                              |
                                              | (One2OneChannel <GyroData>)
                                              v
                                    +-----------------+
                                    |  DataConsumer   | (Consumer Process)
                                    +-----------------+
                                              |
                                              v
                                       [ UART Console ]
```
