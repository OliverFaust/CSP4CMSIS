#include "csp/csp4cmsis.h"
#include <cstdio>

extern "C" void csp4cmsis_fatal_error(const char* message) {
    printf("\r\n%s\r\n", message);    // e.g. "CSP4CMSIS: rendezvous channel: second ALTing reader ..."
    for (;;) { }
}
