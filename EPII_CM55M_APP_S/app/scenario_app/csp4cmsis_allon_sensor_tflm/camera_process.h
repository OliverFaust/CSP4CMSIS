#ifndef CAMERA_PROCESS_HPP
#define CAMERA_PROCESS_HPP

#include <csp/csp4cmsis.h>
#include "common_types.h"
#include "cisdp_sensor.h"

// API 1.3: stack depth (words) is now fixed at compile time via
// CSProcessStatic<N> -- 256 matches the value this process previously
// received as CSP_LEGACY_PARALLEL_STACK_WORDS's dynamic fallback, so
// its memory footprint is unchanged; only where it lives (static
// storage vs. heap) has changed.
class Camera : public csp::CSProcessStatic<256> {
public:
    Camera(csp::Chanout<frame_t> out);
    void run() override;
    const char* name() const override { return "Camera"; }

private:
    csp::Chanout<frame_t> m_frame_out;
    uint32_t m_frame_counter;
};

#ifdef __cplusplus
extern "C" {
#endif
    void os_app_dplib_cb(SENSORDPLIB_STATUS_E event);
#ifdef __cplusplus
}
#endif

#endif