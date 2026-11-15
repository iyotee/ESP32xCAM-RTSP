// OV2640 capture through the esp32-camera driver bundled with Arduino-ESP32.
#pragma once

#include <esp_camera.h>

// Owns one driver frame buffer and returns it exactly once (RAII, move-only).
class CameraFrame {
 public:
  CameraFrame() = default;
  explicit CameraFrame(camera_fb_t *fb) : fb_(fb) {}
  ~CameraFrame() { release(); }
  CameraFrame(CameraFrame &&other) noexcept : fb_(other.fb_) { other.fb_ = nullptr; }
  CameraFrame &operator=(CameraFrame &&other) noexcept {
    if (this != &other) {
      release();
      fb_ = other.fb_;
      other.fb_ = nullptr;
    }
    return *this;
  }
  CameraFrame(const CameraFrame &) = delete;
  CameraFrame &operator=(const CameraFrame &) = delete;

  explicit operator bool() const { return fb_ != nullptr; }
  const camera_fb_t *operator->() const { return fb_; }

  void release() {
    if (fb_ != nullptr) {
      esp_camera_fb_return(fb_);
      fb_ = nullptr;
    }
  }

 private:
  camera_fb_t *fb_ = nullptr;
};

namespace camera {

// Powers up and configures the sensor. Safe to call again after end().
bool begin();
// Stops DMA and frees the driver buffers (used before an OTA update).
void end();
bool running();
// Blocks at most one sensor frame period when GRAB_LATEST has nothing ready.
CameraFrame grab();

}  // namespace camera
