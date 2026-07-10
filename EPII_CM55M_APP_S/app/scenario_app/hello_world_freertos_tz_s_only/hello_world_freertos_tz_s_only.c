#include "hello_world_freertos_tz_s_only.h"

#ifdef FREERTOS
/* FreeRTOS kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#endif

#ifdef TRUSTZONE_SEC
#if (__ARM_FEATURE_CMSE & 1) == 0
#error "Need ARMv8-M security extensions"
#elif (__ARM_FEATURE_CMSE & 2) == 0
#error "Compile with --cmse"
#endif
#include "arm_cmse.h"
#ifdef NSC
#include "veneer_table.h"
#endif
#endif

#include "xprintf.h"

// Core Driver Hardware Abstractions
#include "hx_drv_scu.h"
#include "hx_drv_iic.h" 

/* Task priorities. */
#define hello_task1_PRIORITY	(configMAX_PRIORITIES - 1)
#define hello_task2_PRIORITY	(configMAX_PRIORITIES - 1)

// ADXL345 Configuration Constants
#define ADXL345_SLAVE_ADDR      0x53   
#define ADXL345_REG_DEVID       0x00   
#define ADXL345_EXPECTED_ID     0xE5   

/*******************************************************************************
 * Hardware Synchronization
 ******************************************************************************/
static volatile bool i2c_transact_done = false;

// Callback fired by the I2C interrupt when the physical bus finishes
void i2c_callback(void)
{
    i2c_transact_done = true;
}

/*******************************************************************************
 * Definitions
 ******************************************************************************/
static void hello_task1(void *pvParameters);
static void hello_task2(void *pvParameters);

/*******************************************************************************
 * Code
 ******************************************************************************/
int app_main(void)
{
    printf("Initializing System for ADXL345 Read...\r\n");

    hx_drv_scu_set_PA2_pinmux(SCU_PA2_PINMUX_I2C_M_SCL, 1);
    hx_drv_scu_set_PA3_pinmux(SCU_PA3_PINMUX_I2C_M_SDA, 1);

    int i2c_status = hx_drv_i2cm_init(USE_DW_IIC_0, HX_I2C_HOST_MST_0_BASE, DW_IIC_SPEED_FAST);
    if (i2c_status != 0)
    {
        printf("I2C Master Initialization Failed! Error: %d\r\n", i2c_status);
        while(1);
    }

    if ( xTaskCreate(hello_task1, "Hello_task1", 512, NULL, hello_task1_PRIORITY, NULL) != pdPASS )
    {
        printf("Hello_task1 creation failed!.\r\n");
        while (1);
    }

    if ( xTaskCreate(hello_task2, "Hello_task2", 512, NULL, hello_task2_PRIORITY, NULL) != pdPASS )
    {
        printf("Hello_task2 creation failed!.\r\n");
        while (1);
    }

    vTaskStartScheduler();
    for (;;);
}

static void hello_task1(void *pvParameters)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void hello_task2(void *pvParameters)
{
    uint8_t reg_addr = ADXL345_REG_DEVID;
    uint8_t dev_id_val = 0;
    int status;

    for (;;)
    {
        printf("\r\n--- Reading ADXL345 Device ID ---\r\n");
        
        // Reset flag before starting
        i2c_transact_done = false;
        
        // Step 1: Write Phase (Tell sensor which register we want)
        // Pass our callback function so the driver notifies us when finished
        status = hx_drv_i2cm_interrupt_write(USE_DW_IIC_0, ADXL345_SLAVE_ADDR, &reg_addr, 1, (void *)i2c_callback);
        
        if (status != 0)
        {
            printf("Failed to start register write. Error: %d\r\n", status);
        }
        else
        {
            // Block this task and let the OS run until the hardware ISR fires the callback
            while (!i2c_transact_done)
            {
                vTaskDelay(pdMS_TO_TICKS(1)); 
            }

            // Write is fully complete on the physical bus. Safe to proceed.
            i2c_transact_done = false; 
            dev_id_val = 0;

            // Step 2: Read Phase
            status = hx_drv_i2cm_interrupt_read(USE_DW_IIC_0, ADXL345_SLAVE_ADDR, &dev_id_val, 1, (void *)i2c_callback);
            
            if (status == 0)
            {
                // Wait for the read transaction to complete
                while (!i2c_transact_done)
                {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }

                printf("Successfully read Register 0x00: 0x%02X\r\n", dev_id_val);
                
                if (dev_id_val == ADXL345_EXPECTED_ID)
                {
                    printf("Verification Pass: Valid ADXL345 detected.\r\n");
                }
                else
                {
                    printf("Verification Fail: Unexpected Device ID value!\r\n");
                }
            }
            else
            {
                printf("Failed to start register read. Error: %d\r\n", status);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
