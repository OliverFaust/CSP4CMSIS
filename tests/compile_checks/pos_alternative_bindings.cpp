// EXPECT-OK: pipe bindings and a RelTimeoutGuard lvalue bind by reference (no copy)
#include "csp/csp4cmsis.h"
void f() {
    static csp::Channel<int> ch;
    static csp::BufferedChannel<int, 4, csp::BufferPolicy::KeepNewest> bc;
    auto in = ch.reader(); auto bin = bc.reader(); int v = 0, w = 0;
    csp::RelTimeoutGuard to(csp::Time(5));
    csp::Alternative alt(in | v, bin | w, to);
    (void)alt.priSelect();
}
