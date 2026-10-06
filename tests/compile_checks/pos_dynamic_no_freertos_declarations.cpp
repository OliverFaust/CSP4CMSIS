// EXPECT-OK: with CSP4CMSIS_DYNAMIC_ALLOCATION, csp4cmsis.h declares nothing of FreeRTOS
#undef CSP4CMSIS_STATIC_ALLOCATION
#define CSP4CMSIS_DYNAMIC_ALLOCATION 1
#include "csp/csp4cmsis.h"
#if defined(INC_FREERTOS_H) || defined(INC_TASK_H) || defined(tskKERNEL_VERSION_NUMBER)
#error "FreeRTOS declarations visible through csp4cmsis.h"
#endif
struct P : csp::CSProcessStatic<256> { void run() override {} };
void f() { static P p; csp::Run(csp::InParallel(p), csp::ExecutionMode::StaticNetwork); }
