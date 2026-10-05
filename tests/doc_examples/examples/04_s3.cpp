#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

class Worker : public CSProcessStatic<CSP_TYPICAL_STACK_WORDS> {
    int id;
public:
    explicit Worker(int i) : id(i) {}
    void run() override { printf("worker %d done\r\n", id); }   // returning ends the thread
};

static Worker first(1), second(2);

// Called from a thread: returns when both workers have returned from run().
void run_workers(void) {
    Run(InParallel(first, second), ExecutionMode::TerminatingNetwork, osPriorityNormal);
}
