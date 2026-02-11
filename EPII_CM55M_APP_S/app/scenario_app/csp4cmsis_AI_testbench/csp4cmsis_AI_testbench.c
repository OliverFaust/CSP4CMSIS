#include "csp4cmsis_AI_testbench.h"
#include "xprintf.h"
#include "WE2_debug.h"
#include "hx_drv_scu.h"
#include "hx_drv_swreg_aon.h"
#include "driver_interface.h"
#ifdef IP_sensorctrl
#include "hx_drv_sensorctrl.h"
#endif
#ifdef IP_xdma
#include "hx_drv_xdma.h"
#include "sensor_dp_lib.h"
#endif
#ifdef IP_cdm
#include "hx_drv_cdm.h"
#endif
#ifdef IP_edm
#include "hx_drv_edm.h"
#endif
#ifdef IP_gpio
#include "hx_drv_gpio.h"
#endif
#ifdef IP_swreg_aon
#include "hx_drv_swreg_aon.h"
#endif
#include "hx_drv_pmu.h"
#include "powermode.h"
#ifdef FREERTOS
/* FreeRTOS kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#endif
#include "hx_drv_CIS_common.h"
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
/* Trustzone config. */

#ifndef TRUSTZONE_SEC_ONLY
/* FreeRTOS includes. */
#include "secure_port_macros.h"
#endif
#endif

#include "common_config.h"
#include "cvapp.h"
#include "sleep_mode.h"
#include "pinmux_cfg.h"

#define CIS_XSHUT_SGPIO0
#ifdef CIS_XSHUT_SGPIO0
#define DEAULT_XHSUTDOWN_PIN    AON_GPIO2
#else
#define DEAULT_XHSUTDOWN_PIN    AON_GPIO2
#endif

/*******************************************************************************
 * Code
 ******************************************************************************


/*!
 * @brief Main function
 */
int app_main(void)
{
	uint32_t wakeup_event;
	uint32_t wakeup_event1;

	hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT, &wakeup_event);
	hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT1, &wakeup_event1);
        xprintf("Wakeup_event=0x%x,WakeupEvt1=0x%x\n", wakeup_event, wakeup_event1);
        RunProcessingChainTest();
        vTaskStartScheduler();

        for (;;)
            ;
            
    return 0;
}


