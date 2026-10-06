// EXPECT-OK: no backend define and RTE_Components.h silent: FreeRTOS detected from FreeRTOS.h
// CONTEXT: FreeRTOS
#undef CSP4CMSIS_RTOS2_BACKEND_FREERTOS
#undef CSP4CMSIS_RTOS2_BACKEND_RTX5
#if __has_include("RTE_Components.h")
#include "RTE_Components.h"
#undef RTE_CMSIS_RTOS2_FreeRTOS
#undef RTE_CMSIS_RTOS2_RTX5
#endif
#include "csp/csp4cmsis.h"
#if !defined(CSP4CMSIS_RTOS2_BACKEND_FREERTOS)
#error "FreeRTOS not detected from FreeRTOS.h"
#endif
