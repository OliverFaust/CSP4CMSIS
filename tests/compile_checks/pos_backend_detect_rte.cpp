// EXPECT-OK: no backend define: detected from RTE_Components.h (or the RTOS header)
#undef CSP4CMSIS_RTOS2_BACKEND_FREERTOS
#undef CSP4CMSIS_RTOS2_BACKEND_RTX5
#include "csp/csp4cmsis.h"
#if !defined(CSP4CMSIS_STATIC_ALLOCATION)
#error "static allocation is not the default"
#endif
#if defined(RTE_CMSIS_RTOS2_FreeRTOS) && !defined(CSP4CMSIS_RTOS2_BACKEND_FREERTOS)
#error "FreeRTOS not detected"
#endif
#if defined(RTE_CMSIS_RTOS2_RTX5) && !defined(CSP4CMSIS_RTOS2_BACKEND_RTX5)
#error "RTX5 not detected"
#endif
