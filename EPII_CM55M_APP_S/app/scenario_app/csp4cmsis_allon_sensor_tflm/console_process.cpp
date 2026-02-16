// console_process.cpp
#include "console_process.h"
#include "xprintf.h"

Console::Console(csp::Chanin<result_t> in)
    : m_result_in(in)
{
}

void Console::run()
{
    while (true) {
        result_t res;
        m_result_in >> res;
        xprintf("Frame %lu: prediction = %d\n", res.frame_index, res.prediction);
    }
}
