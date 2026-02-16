#include "camera_process.h"
#include "cisdp_cfg.h"
#include "WE2_debug.h"

using namespace csp;

static Channel<trigger_t> g_trigger_chan;

extern "C" void os_app_dplib_cb(SENSORDPLIB_STATUS_E event)
{
    if (event == SENSORDPLIB_STATUS_XDMA_FRAME_READY) {
        dbg_printf(DBG_MORE_INFO, "Frame ready IRQ\r\n");
        bool sent = g_trigger_chan.writer().putFromISR(trigger_t{});
        dbg_printf(DBG_MORE_INFO, "Trigger send\r\n");
        if(!sent) {
            // This might happen if the Camera task is too slow
            // and the channel is unbuffered.
        }
    }
}

Camera::Camera(Chanout<frame_t> out)
    : m_frame_out(out), m_frame_counter(0) {}

void Camera::run()
{
    trigger_t t;
    // Get the reader interface for our private channel
    auto trigger_reader = g_trigger_chan.reader();    
    xprintf("Camera: initializing sensor\e\n");
    if (cisdp_sensor_init(true) < 0) {
        dbg_printf(DBG_LESS_INFO, "Camera: sensor init failed\r\n");
        return;
    }

    if (cisdp_dp_init(true,
                      SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG,
                      os_app_dplib_cb,
                      0,
                      APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
        dbg_printf(DBG_LESS_INFO, "Camera: data path init failed\r\n");
        return;
    }

    cisdp_sensor_start();
    //dbg_printf(DBG_LESS_INFO, "Camera: streaming started\r\n");

    //vTaskDelay(pdMS_TO_TICKS(100));

    while (true) {
        
        //dbg_printf(DBG_MORE_INFO, "Waiting for trigger\r\n");
        trigger_reader.read(t);
        //dbg_printf(DBG_MORE_INFO, "Camera: trigger received\r\n");

        uint32_t jpeg_addr, jpeg_sz;
        cisdp_get_jpginfo(&jpeg_sz, &jpeg_addr);

        frame_t f;
        f.index = m_frame_counter++;
        f.jpeg_addr = jpeg_addr;
        f.jpeg_sz = jpeg_sz;

        m_frame_out.write(f);
        //dbg_printf(DBG_MORE_INFO, "Camera: frame %lu sent\r\n", f.index);
        dbg_printf(DBG_MORE_INFO, "Camera: retrigger hardware for next frame\r\n");
        sensordplib_retrigger_capture(); 
    }
}
