#ifndef CAMERA_PROCESS_HPP
#define CAMERA_PROCESS_HPP

#include <csp/csp4cmsis.h>
#include "common_types.h"
#include "cisdp_sensor.h"

class Camera : public csp::CSProcess {
public:
    Camera(csp::Chanout<frame_t> out);
    void run() override;

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