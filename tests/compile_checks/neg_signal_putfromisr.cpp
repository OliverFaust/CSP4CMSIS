// EXPECT-ERROR: no member named 'putFromISR'
// Replaces FVP test T16s: a signal channel's writer end cannot be used from an ISR.
#include "csp/csp4cmsis.h"
void isr() {
    static csp::SignalChannel<> sig;
    auto out = sig.writer();
    (void)out.putFromISR(csp::Signal{});
}
