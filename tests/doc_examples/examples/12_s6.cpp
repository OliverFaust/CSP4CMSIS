#include "csp/csp4cmsis.h"

// For the debugger.
const char* volatile app_fatal_message = nullptr;

extern "C" void csp4cmsis_fatal_error(const char* message) {
    __disable_irq();
    app_fatal_message = message;      // e.g. "CSP4CMSIS: rendezvous channel: second ALTing reader ..."
    for (;;) { }
}
