// EXPECT-OK: signal channel as a data-less rendezvous: plain and ALT use
#include "csp/csp4cmsis.h"
void f() {
    static csp::SignalChannel<> sig;
    static csp::SamplingBufferedChannel<unsigned, 1, csp::BufferPolicy::KeepNewest> latest;
    auto out = sig.writer(); auto in = sig.reader(); csp::Signal s;
    out << csp::Signal{};
    in >> s;
    auto lin = latest.reader(); unsigned v = 0;
    csp::Alternative alt(in | s, lin | v);
    (void)alt.priSelect();
}
