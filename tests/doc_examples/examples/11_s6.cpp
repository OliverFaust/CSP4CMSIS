#include "csp/csp4cmsis.h"

using namespace csp;

static Channel<int>    numbers;
static SignalChannel<> stop;

class Counter : public CSProcessStatic<256> {
    Chanout<int>   out  = numbers.writer();
    Chanin<Signal> quit = stop.reader();
public:
    void run() override {
        int next = 0;
        Signal s;
        Alternative alt(quit | s, out | next);         // output guard: out | value
        while (alt.priSelect() == 1) {                 // stop wins if both are ready
            ++next;                                    // `next` was written; offer the next one
        }
    }
};
