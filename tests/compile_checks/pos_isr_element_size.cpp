// EXPECT-OK: 64-byte ISR writer (Block and KeepNewest); a large channel used only by tasks is not limited
#include "csp/csp4cmsis.h"
struct Max { unsigned char b[64]; };
struct Frame { unsigned char b[1024]; };
void isr() {
    static csp::BufferedChannel<Max, 2> ch;
    static csp::BufferedChannel<Max, 1, csp::BufferPolicy::KeepNewest> latest;
    Max v = {};
    (void)ch.isrWriter().putFromISR(v);
    (void)latest.isrWriter().putFromISR(v);
}
void task() {
    static csp::BufferedChannel<Frame, 2> big;
    auto out = big.writer(); static Frame f = {};
    out << f;
}
