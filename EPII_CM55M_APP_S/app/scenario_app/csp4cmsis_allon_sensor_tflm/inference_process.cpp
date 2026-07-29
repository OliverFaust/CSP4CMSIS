#include "inference_process.h"
#include "cvapp.h"          // for cv_init, cv_run
#include "xprintf.h"
#include "WE2_debug.h"

using namespace csp;

Inference::Inference(Chanin<frame_t> in, Chanout<result_t> out)
    : m_frame_in(in), m_result_out(out) {}

void Inference::run()
{
    if (cv_init(true, true) < 0) {
        xprintf("Inference: model init failed\n");
        return;
    }

    while (true) {
        frame_t f;
        m_frame_in.read(f);

        // cv_run() reads directly from the raw sensor buffer, not the
        // JPEG buffer -- a model expecting JPEG input would need a
        // decode step here first.
        int8_t score = cv_run();

        result_t res;
        res.frame_index = f.index;
        res.prediction = score;

        m_result_out.write(res);

        dbg_printf(DBG_MORE_INFO, "Frame %lu: prediction = %d\n", f.index, score);
    }
}
