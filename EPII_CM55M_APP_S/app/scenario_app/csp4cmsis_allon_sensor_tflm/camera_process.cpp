#include "camera_process.h"
#include "cisdp_cfg.h"
#include "WE2_debug.h"

using namespace csp;

static Channel < trigger_t > g_trigger_chan;

extern "C"
void os_app_dplib_cb(SENSORDPLIB_STATUS_E event) {
  // No print here: xprintf() has no locking, and this callback 
  // runs in interrupt context, where a mutex-based fix isn't an 
  // option (you can't block on a mutex from an ISR). Calling it 
  // here can tear another task's in-progress output mid-string. 
  // If this event needs to be visible, signal it through an 
  // ISR-safe path instead -- a counter or a lock-free queue a 
  // task can drain and print from.
  if (event == SENSORDPLIB_STATUS_XDMA_FRAME_READY) {
    bool sent = g_trigger_chan.writer().putFromISR(trigger_t {});
    if (!sent) {
      // Camera task was too slow to keep up with an unbuffered channel.
    }
  }
}

Camera::Camera(Chanout < frame_t > out): m_frame_out(out), m_frame_counter(0) {}

void Camera::run() {
  trigger_t t;
  auto trigger_reader = g_trigger_chan.reader();
  xprintf("Camera: initializing sensor\r\n");
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

  while (true) {
    trigger_reader.read(t);

    uint32_t jpeg_addr, jpeg_sz;
    cisdp_get_jpginfo( & jpeg_sz, & jpeg_addr);

    frame_t f;
    f.index = m_frame_counter++;
    f.jpeg_addr = jpeg_addr;
    f.jpeg_sz = jpeg_sz;

    m_frame_out.write(f);
    dbg_printf(DBG_MORE_INFO, "Camera: retrigger hardware for next frame\r\n");
    sensordplib_retrigger_capture();
  }
}
