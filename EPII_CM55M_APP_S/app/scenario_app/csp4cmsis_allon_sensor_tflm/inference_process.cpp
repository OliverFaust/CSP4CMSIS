#include "inference_process.h"
#include "cvapp.h"          // for cv_init, cv_run
#include "xprintf.h"
#include "WE2_debug.h"

using namespace csp;

Inference::Inference(Chanin<frame_t> in, Chanout<result_t> out)
    : m_frame_in(in), m_result_out(out) {}

void Inference::run()
{
    // Initialise model once
    if (cv_init(true, true) < 0) {
        xprintf("Inference: model init failed\n");
        return;
    }

    while (true) {
        frame_t f;
        m_frame_in.read(f);                  // wait for a frame

        // Invalidate cache if needed
        // hx_InvalidateDCache_by_Addr((void*)f.jpeg_addr, f.jpeg_sz);

        // The model expects raw YUV input. In the original code, cv_run()
        // automatically uses the raw buffer (app_get_raw_addr()), so we don't
        // need to pass the JPEG buffer. Instead, we simply run inference.
        // If your model needs JPEG decoding, you'd call a decoder here.
        int8_t score = cv_run();              // runs on the latest raw frame

        result_t res;
        res.frame_index = f.index;
        res.prediction = score;

        m_result_out.write(res);

        dbg_printf(DBG_MORE_INFO, "Frame %lu: prediction = %d\n", f.index, score);
    }
}
