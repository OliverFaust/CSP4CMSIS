// EXPECT-ERROR: configSUPPORT_STATIC_ALLOCATION 1
// CONTEXT: FreeRTOS
// 3.0: static allocation (the default) with configSUPPORT_STATIC_ALLOCATION 0 is
// reported by the library source glue.cpp (the headers do not check it).
#include "FreeRTOS.h"
#undef configSUPPORT_STATIC_ALLOCATION
#define configSUPPORT_STATIC_ALLOCATION 0
#include "../../csp4cmsis/src/glue.cpp"
