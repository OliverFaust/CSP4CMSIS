// EXPECT-ERROR: no member named 'putFromISR'
// Replaces FVP tests T15i and T16a: no ISR write path into a rendezvous channel.
#include "csp/csp4cmsis.h"
void isr() {
    static csp::Channel<unsigned> ch;
    auto out = ch.writer();
    (void)out.putFromISR(1u);
}
