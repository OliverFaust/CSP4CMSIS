#ifndef INFERENCE_PROCESS_HPP
#define INFERENCE_PROCESS_HPP

#include <csp/csp4cmsis.h>
#include "common_types.h"

class Inference : public csp::CSProcess {
public:
    Inference(csp::Chanin<frame_t> in, csp::Chanout<result_t> out);
    void run() override;

private:
    csp::Chanin<frame_t>  m_frame_in;
    csp::Chanout<result_t> m_result_out;
};

#endif
