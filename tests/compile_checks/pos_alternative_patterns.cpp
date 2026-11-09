// EXPECT-OK: 3.1.0 use patterns of Alternative compile unchanged (3.2.0); several Alternatives on one end
#include "csp/csp4cmsis.h"
static csp::Channel<int> ch;
static csp::BufferedChannel<int, 1> cmd_chan;
int pick(csp::Alternative& alt) { return alt.priSelect(); }      // parameter
struct Proc {
    csp::Chanin<int> in = ch.reader();
    csp::Chanin<int> cmd_in = cmd_chan.reader();
    int v = 0, cmd = 0;
    csp::RelTimeoutGuard skip{csp::Time(0)};
    csp::Alternative poll{cmd_in | cmd, skip};                    // member, inline bindings
    csp::Alternative later;                                       // member, default-constructed
    Proc() { later.addBinding(in | v); later.addBinding(skip); }
    void step() {
        (void)pick(poll); (void)later.fairSelect();
        int c = 0;                                                // same end, another variable
        csp::RelTimeoutGuard pace(csp::Time(1));
        csp::Alternative wait(cmd_in | c, pace);
        (void)pick(wait);
    }
};
void f() { static Proc p; p.step(); }
