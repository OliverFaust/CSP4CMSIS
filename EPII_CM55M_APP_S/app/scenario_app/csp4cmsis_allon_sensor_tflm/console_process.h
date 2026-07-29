#ifndef CONSOLE_PROCESS_H
#define CONSOLE_PROCESS_H

#include <csp/csp4cmsis.h>
#include "common_types.h"    // for frame_t and result_t

// Forward declaration of result_t – the full definition will be included in the .cpp file.
struct result_t;

// API 1.3: see camera_process.h for why 256 and why CSProcessStatic<N>.
class Console : public csp::CSProcessStatic<256> {
public:
    explicit Console(csp::Chanin<result_t> in);
    void run() override;
    const char* name() const override { return "Console"; }

private:
    csp::Chanin<result_t> m_result_in;
};

#endif // CONSOLE_PROCESS_H