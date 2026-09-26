// EXPECT-ERROR: no member named 'isrWriter'
// Rendezvous channels have no ISR writer end (T15i, T16a).
#include "csp/csp4cmsis.h"
void isr() {
    static csp::Channel<unsigned> ch;
    (void)ch.isrWriter();
}
