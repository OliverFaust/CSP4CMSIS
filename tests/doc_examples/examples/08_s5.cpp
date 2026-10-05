#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

static SamplingBufferedChannel<uint32_t, 1, BufferPolicy::KeepNewest> presses;

// Called from the button's interrupt handler (STM32: from the EXTI callback).
extern "C" void button_pressed_from_isr(void) {
    static uint32_t count = 0;
    (void)presses.isrWriter().putFromISR(++count);     // KeepNewest: always true
}

class ButtonHandler : public CSProcessStatic<256> {
    Chanin<uint32_t> in = presses.reader();
public:
    void run() override {
        uint32_t count;
        while (true) {
            in >> count;                               // the latest press
            printf("button: %lu presses so far\r\n", (unsigned long)count);
        }
    }
};

// Before the interrupt is enabled (NUCLEO-G474RE user button: EXTI15_10_IRQn).
void allow_csp4cmsis_calls(IRQn_Type irq) {
    NVIC_SetPriority(irq, CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY);
}
