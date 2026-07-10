#include "csp/csp4cmsis.h"
#include <cstdio>
#include <cstdlib> // For abs()
#include "FreeRTOS.h"
#include "task.h"

// Bring in the Himax C drivers
extern "C" {
#include "hx_drv_scu.h"
#include "hx_drv_iic.h"
}

using namespace csp;

// --- Hardware Synchronization Setup ---
static volatile bool i2c_transact_done = false;

extern "C" void i2c_callback(void) {
    i2c_transact_done = true;
}

// --- Data Structures ---
struct AccelData {
    int16_t x;
    int16_t y;
    int16_t z;
};

// --- 1. ADXL345 Reader Process ---
class Adxl345Reader : public CSProcess {
    Chanout<AccelData> out;
    const uint8_t slave_addr = 0x53;

    // Helper method to write to a single register
    void write_register(uint8_t reg, uint8_t val) {
        uint8_t data[2] = {reg, val};
        i2c_transact_done = false;
        hx_drv_i2cm_interrupt_write(USE_DW_IIC_0, slave_addr, data, 2, (void *)i2c_callback);
        while (!i2c_transact_done) { vTaskDelay(pdMS_TO_TICKS(1)); }
    }

    // Helper method to read multiple contiguous bytes
    void read_registers(uint8_t reg, uint8_t* buffer, uint8_t len) {
        // Step 1: Write pointer
        i2c_transact_done = false;
        hx_drv_i2cm_interrupt_write(USE_DW_IIC_0, slave_addr, &reg, 1, (void *)i2c_callback);
        while (!i2c_transact_done) { vTaskDelay(pdMS_TO_TICKS(1)); }

        // Step 2: Read sequence
        i2c_transact_done = false;
        hx_drv_i2cm_interrupt_read(USE_DW_IIC_0, slave_addr, buffer, len, (void *)i2c_callback);
        while (!i2c_transact_done) { vTaskDelay(pdMS_TO_TICKS(1)); }
    }

public:
    Adxl345Reader(Chanout<AccelData> w) : out(w) {}
    
    void run() override {
        printf("[Reader] Initializing ADXL345...\r\n");
        
        // 0x2D (POWER_CTL): Set Measure bit (bit 3) to 1 to wake up sensor
        write_register(0x2D, 0x08);
        
        // 0x31 (DATA_FORMAT): Set to Full Resolution, +/- 2g range
        write_register(0x31, 0x08);

        uint8_t raw_buffer[6];
        AccelData reading;

        while (true) {
            // Read 6 bytes starting at 0x32 (DATAX0). This fetches X, Y, and Z.
            read_registers(0x32, raw_buffer, 6);

            // Reconstruct the 16-bit values (Little Endian format)
            reading.x = (int16_t)((raw_buffer[1] << 8) | raw_buffer[0]);
            reading.y = (int16_t)((raw_buffer[3] << 8) | raw_buffer[2]);
            reading.z = (int16_t)((raw_buffer[5] << 8) | raw_buffer[4]);

            // Emit raw data down the channel
            out << reading;

            // Poll at ~20Hz
            vTaskDelay(pdMS_TO_TICKS(50)); 
        }
    }
};

// --- 2. Shake Math Logic Process ---
class ShakeLogic : public CSProcess {
    Chanin<AccelData> in;
    Chanout<bool> out;

public:
    ShakeLogic(Chanin<AccelData> r, Chanout<bool> w) : in(r), out(w) {}
    
    void run() override {
        AccelData current;
        AccelData previous = {0, 0, 0};
        bool is_first_reading = true;
        
        // The higher this number, the harder the shake required to trigger.
        // Needs tuning based on your specific physical setup.
        const int SHAKE_THRESHOLD = 300; 

        while (true) {
            // Block until new sensor data arrives
            in >> current;

            if (!is_first_reading) {
                // Calculate the absolute delta (change) on each axis
                int delta_x = abs(current.x - previous.x);
                int delta_y = abs(current.y - previous.y);
                int delta_z = abs(current.z - previous.z);
                
                // Sum the deltas to get total movement magnitude approximation
                int total_movement = delta_x + delta_y + delta_z;

                if (total_movement > SHAKE_THRESHOLD) {
                    // Send an event flag down the line!
                    out << true; 
                }
            } else {
                is_first_reading = false;
            }

            previous = current;
        }
    }
};

// --- 3. Shake Event Consumer Process ---
// A CSP channel requires a receiver. This process simply listens for shake events.
class ShakeEventConsumer : public CSProcess {
    Chanin<bool> in;

public:
    ShakeEventConsumer(Chanin<bool> r) : in(r) {}

    void run() override {
        bool shake_flag;
        while (true) {
            // Block until a shake event is emitted
            in >> shake_flag;
            
            if (shake_flag) {
                printf("\r\n============================\r\n");
                printf(" !!! SHAKE DETECTED !!! \r\n");
                printf("============================\r\n");
            }
        }
    }
};

// --- 4. Main App Task ---
void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Create the connecting channels
    static Channel<AccelData> c_accel_data;
    static Channel<bool> c_shake_events;

    // Instantiate the processes, wiring them together
    static Adxl345Reader      proc_reader(c_accel_data.writer());
    static ShakeLogic         proc_logic(c_accel_data.reader(), c_shake_events.writer());
    static ShakeEventConsumer proc_consumer(c_shake_events.reader());

    // Execute the network
    Run(
        InParallel(proc_reader, proc_logic, proc_consumer),
        ExecutionMode::StaticNetwork
    );
}

void RunProcessingChainTest(void) {
    // Note: Task creation is the only 'dynamic' part remaining, standard for FreeRTOS
    xTaskCreate(MainApp_Task, "MainApp", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
}
