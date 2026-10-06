// EXPECT-ERROR: could not be detected
// CONTEXT: FreeRTOS
// EXTRA-FLAGS: -I{here}/fake_rtos
// 3.0: no backend define, RTE_Components.h does not decide, and both FreeRTOS.h and
// rtx_os.h are reachable: the backend must be defined explicitly.
#undef CSP4CMSIS_RTOS2_BACKEND_FREERTOS
#undef CSP4CMSIS_RTOS2_BACKEND_RTX5
#if __has_include("RTE_Components.h")
#include "RTE_Components.h"
#undef RTE_CMSIS_RTOS2_FreeRTOS
#undef RTE_CMSIS_RTOS2_RTX5
#endif
#include "csp/csp4cmsis.h"
