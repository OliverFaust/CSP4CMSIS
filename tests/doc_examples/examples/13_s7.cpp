#include "csp/csp4cmsis.h"

using namespace csp;

static Barrier step(3);                 // three processes per step

class Stage : public CSProcessStatic<256> {
public:
    void run() override {
        while (true) {
            // ... this stage's share of the step ...
            step.sync();                // wait for the other two stages
        }
    }
};

static Stage s1, s2, s3;

void start_stages(void) {
    Run(InParallel(s1, s2, s3), ExecutionMode::StaticNetwork);
}
