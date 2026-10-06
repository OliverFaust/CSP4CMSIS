#include "csp/csp4cmsis.h"

using namespace csp;

static BufferedChannel<bool, 1> transfer_done;

extern "C" void start_transfer(void);   // application: starts one interrupt-driven transfer

// Called from the transfer-complete interrupt (STM32 HAL: e.g. HAL_UART_TxCpltCallback).
extern "C" void transfer_complete_from_isr(void) {
    if (!transfer_done.isrWriter().putFromISR(true)) {
        // A second completion before the first was read: stop (section 6, fatal-error hook).
        csp4cmsis_fatal_error("transfer completion lost");
    }
}

class Transmitter : public CSProcessStatic<256> {
    Chanin<bool> done = transfer_done.reader();
public:
    void run() override {
        bool ok;
        while (true) {
            start_transfer();
            done >> ok;                 // kept even if it arrived before this read
            SleepFor(Seconds(1));
        }
    }
};
