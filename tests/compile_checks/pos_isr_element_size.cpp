// EXPECT-OK: 64-byte ISR write compiles; a large channel used only by tasks is not limited
#include "csp/csp4cmsis.h"
struct Max { unsigned char b[64]; };
struct Frame { unsigned char b[1024]; };
void isr() {
    static csp::SamplingBufferedChannel<Max, 2> ch;
    auto out = ch.writer(); Max v = {};
    (void)out.putFromISR(v);
}
void task() {
    static csp::SamplingBufferedChannel<Frame, 2> big;
    auto out = big.writer(); static Frame f = {};
    out << f;
}
