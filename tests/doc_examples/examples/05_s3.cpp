#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

class Blinker : public CSProcessStatic<256> {
public:
    const char* name() const override { return "Blinker"; }
    void run() override { while (true) { SleepFor(Milliseconds(500)); } }
};

class Heartbeat : public CSProcessStatic<256> {
public:
    const char* name() const override { return "Heartbeat"; }
    void run() override { while (true) { SleepFor(Seconds(1)); } }
};

static Blinker blinker;
static Heartbeat heartbeat;

// Body of a monitoring thread (e.g. CubeMX's defaultTask).
void monitor(void) {
    auto network = InParallel(blinker, heartbeat);
    Run(network, ExecutionMode::StaticNetwork);
    while (true) {
        SleepFor(Seconds(5));
        network.forEachProcess([](CSProcess& p) {
            uint32_t free_words = p.stackHighWaterMarkWords();
            if (free_words != CSP_STACK_HWM_UNAVAILABLE) {
                printf("%s: %lu of %lu words never used\r\n", p.name(),
                       (unsigned long)free_words, (unsigned long)p.stackWords());
            }
        });
    }
}
