// EXPECT-ERROR: exceeds CSP4CMSIS_ISR_MAX_ELEMENT_SIZE
#include "csp/csp4cmsis.h"
struct Big { unsigned char b[65]; };
void isr() {
    static csp::BufferedChannel<Big, 2> ch;
    auto out = ch.isrWriter(); Big v = {};
    (void)out.putFromISR(v);
}
