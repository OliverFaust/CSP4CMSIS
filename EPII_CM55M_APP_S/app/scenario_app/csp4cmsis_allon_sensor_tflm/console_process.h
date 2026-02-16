#ifndef CONSOLE_PROCESS_H
#define CONSOLE_PROCESS_H

#include <csp/csp4cmsis.h>
#include "common_types.h"    // for frame_t and result_t

// Forward declaration of result_t – the full definition will be included in the .cpp file.
struct result_t;

class Console : public csp::CSProcess {
public:
    explicit Console(csp::Chanin<result_t> in);
    void run() override;

private:
    csp::Chanin<result_t> m_result_in;
};

#endif // CONSOLE_PROCESS_H