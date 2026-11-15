#include "Camera.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "board_pins.h"
#include "config.h"

namespace camera {
namespace {

bool gRunning = false;

void applySensorSettings(sensor_t *s) {
  // Neutral picture settings with automatic exposure, gain and white balance.
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 0);
  s->set_gain_ctrl(s, 1);
  s->set_gainceiling(s, GAINCEILING_2X);
  s->set_bpc(s, 0);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
  s->set_hmirror(s, 0);
  s->set_vflip(s, 0);
  s->set_dcw(s, 1);
  s->set_colorbar(s, 0);
}

}  // namespace

bool begin() {
  if (gRunning) return true;

  camera_config_t c = {};  // every field defined: the driver reads them all
  c.pin_pwdn = pins::kPwdn;
  c.pin_reset = pins::kReset;
  c.pin_xclk = pins::kXclk;
  c.pin_sccb_sda = pins::kSiod;
  c.pin_sccb_scl = pins::kSioc;
  c.pin_d7 = pins::kD7;
  c.pin_d6 = pins::kD6;
  c.pin_d5 = pins::kD5;
  c.pin_d4 = pins::kD4;
  c.pin_d3 = pins::kD3;
  c.pin_d2 = pins::kD2;
  c.pin_d1 = pins::kD1;
  c.pin_d0 = pins::kD0;
  c.pin_vsync = pins::kVsync;
  c.pin_href = pins::kHref;
  c.pin_pclk = pins::kPclk;
  c.xclk_freq_hz = cfg::kXclkHz;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = cfg::kFrameSize;
  c.jpeg_quality = cfg::kJpegQuality;
  c.fb_count = cfg::kFrameBufferCount;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  // The driver keeps refilling its buffers: grab() always gets a fresh frame.
  c.grab_mode = CAMERA_GRAB_LATEST;

  if (!psramFound()) {
    log_e("PSRAM not detected: SVGA JPEG frame buffers need it");
    return false;
  }
  const esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    log_e("esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) {
    log_e("camera sensor not available");
    esp_camera_deinit();
    return false;
  }
  applySensorSettings(s);
  gRunning = true;
  log_i("camera ready: PID 0x%02x, framesize %d, quality %d", s->id.PID, int(cfg::kFrameSize),
        cfg::kJpegQuality);
  return true;
}

void end() {
  if (!gRunning) return;
  esp_camera_deinit();
  gRunning = false;
  log_i("camera stopped");
}

bool running() { return gRunning; }

CameraFrame grab() {
  if (!gRunning) return CameraFrame();
  return CameraFrame(esp_camera_fb_get());
}

}  // namespace camera
