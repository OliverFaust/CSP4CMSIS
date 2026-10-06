// EXPECT-ERROR: no member named 'isrWriter'
// Replaces FVP test T16s: signal channels have no ISR writer.
#include "csp/csp4cmsis.h"
void isr() {
    static csp::SignalChannel sig;
    (void)sig.isrWriter();
}
