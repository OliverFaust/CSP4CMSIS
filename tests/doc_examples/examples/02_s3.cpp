#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

struct Reading {
    uint32_t sequence;
    int32_t  value;
};

class Sensor : public CSProcessStatic<256> {   // 256 words = 1 KB of stack
    Chanout<Reading> out;
public:
    explicit Sensor(Chanout<Reading> w) : out(w) {}
    const char* name() const override { return "Sensor"; }
    void run() override {
        for (uint32_t i = 0; ; ++i) {
            out << Reading{i, static_cast<int32_t>(i) * 10};
            SleepFor(100);                     // 100 ticks
        }
    }
};

class Logger : public CSProcessStatic<512> {
    Chanin<Reading> in;
public:
    explicit Logger(Chanin<Reading> r) : in(r) {}
    const char* name() const override { return "Logger"; }
    osPriority_t taskPriority() const override { return osPriorityBelowNormal; }
    void run() override {
        Reading r;
        while (true) {
            in >> r;
            printf("%lu: %ld\r\n", (unsigned long)r.sequence, (long)r.value);
        }
    }
};

static Channel<Reading> readings;
static Sensor sensor(readings.writer());
static Logger logger(readings.reader());

void start_network(void) {
    // Sensor runs at osPriorityLow (the composition priority), Logger at its own.
    Run(InParallel(sensor, logger), ExecutionMode::StaticNetwork, osPriorityLow);
}
