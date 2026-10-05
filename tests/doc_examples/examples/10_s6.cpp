#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

static Channel<int> left, right, merged;

class Merger : public CSProcessStatic<512> {
    Chanin<int>  a   = left.reader();
    Chanin<int>  b   = right.reader();
    Chanout<int> out = merged.writer();
public:
    void run() override {
        int x = 0, y = 0;
        RelTimeoutGuard quiet(Milliseconds(100));      // measured from each select()
        Alternative alt(a | x, b | y, quiet);
        while (true) {
            switch (alt.fairSelect()) {
                case 0: out << x; break;
                case 1: out << y; break;
                case 2: printf("no input for 100 ms\r\n"); break;
            }
        }
    }
};
