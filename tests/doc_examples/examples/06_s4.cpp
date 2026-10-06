#include "csp/csp4cmsis.h"

using namespace csp;

struct Command { uint8_t opcode; uint16_t argument; };    // trivially copyable
struct Sample  { uint32_t timestamp; float value; };

static Channel<Command>          commands;   // rendezvous
static Channel<Command>          requests;   // rendezvous, several writers (one end each)
static SignalChannel             ready;      // rendezvous without data
static BufferedChannel<Sample, 8> samples;   // 8 slots, Block
static BufferedChannel<Sample, 1, BufferPolicy::KeepNewest> latest;     // newest value
static BufferedChannel<Sample, 4, BufferPolicy::KeepOldest> first_four; // first four values

class Producer : public CSProcessStatic<256> {
    Chanout<Sample> out = samples.writer();   // this process's own end
    Chanout<Signal> go  = ready.writer();
public:
    void run() override {
        go << Signal{};                        // waits until the consumer reads it
        for (uint32_t t = 0; ; ++t) {
            out << Sample{t, 0.5f};            // blocks only while all 8 slots are full
            out.write(Sample{t, 1.5f});        // same as <<
        }
    }
};

class Consumer : public CSProcessStatic<256> {
    Chanin<Sample> in    = samples.reader();
    Chanin<Signal> start = ready.reader();
public:
    void run() override {
        Signal s;
        start >> s;
        Sample x;
        while (true) {
            in >> x;                           // blocks while empty
            in.read(x);                        // same as >>
        }
    }
};
