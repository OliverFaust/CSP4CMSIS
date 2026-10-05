// EXPECT-ERROR: override
// CSProcess::endProcess() was removed (2.1.0): an override no longer compiles.
#include "csp/csp4cmsis.h"
struct P : csp::CSProcessStatic<256> {
    void run() override {}
    void endProcess() override {}
};
