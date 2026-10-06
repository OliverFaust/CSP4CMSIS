// EXPECT-OK: 3.0: version macros; the 2.x feature macros are gone
#include "csp/csp4cmsis.h"
#if !defined(CSP4CMSIS_VERSION_MAJOR) || CSP4CMSIS_VERSION_MAJOR != 3 || CSP4CMSIS_VERSION < 30000
#error "version macros"
#endif
#if defined(CSP4CMSIS_ISR_WRITER_API) || defined(CSP4CMSIS_ALT_PROTOCOL_OWRV) || \
    defined(CSP4CMSIS_BUFFERED_CHANNEL_API) || defined(CSP4CMSIS_SLEEPFOR_TIME_API)
#error "a 2.x feature macro is still defined"
#endif
