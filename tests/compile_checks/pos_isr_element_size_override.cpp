// EXPECT-OK: the limit can be raised per project (-D before the headers)
#define CSP4CMSIS_ISR_MAX_ELEMENT_SIZE 128
#include "csp/csp4cmsis.h"
struct Mid { unsigned char b[100]; };
void isr() {
    static csp::SamplingBufferedChannel<Mid, 2> ch;
    auto out = ch.isrWriter(); Mid v = {};
    (void)out.putFromISR(v);
}
