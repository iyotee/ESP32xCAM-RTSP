#include "FrameHub.h"

#include <esp32-hal-log.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "Camera.h"
#include "config.h"

namespace {

constexpr size_t kAllocGranularity = 16 * 1024;
// When the driver reports no ready frame for this long, grab() anyway (it
// blocks at most one sensor frame) rather than starving forever.
constexpr uint64_t kMaxWaitForFrameUs = 500 * 1000;

}  // namespace

FrameSlot *FrameHub::freeSlot() {
  for (FrameSlot &s : slots_) {
    if (s.refs == 0 && &s != latest_) return &s;
  }
  return nullptr;
}

bool FrameHub::store(FrameSlot &slot, const uint8_t *data, size_t length, uint64_t captureUs) {
  if (length > slot.capacity) {
    const size_t capacity = (length + kAllocGranularity - 1) / kAllocGranularity * kAllocGranularity;
    void *grown = heap_caps_realloc(slot.data, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (grown == nullptr) {
      log_e("cannot allocate %u bytes of PSRAM for a frame", unsigned(capacity));
      return false;
    }
    slot.data = static_cast<uint8_t *>(grown);
    slot.capacity = capacity;
  }
  memcpy(slot.data, data, length);
  slot.length = length;
  slot.captureUs = captureUs;
  const rtpjpeg::JpegError err = rtpjpeg::parseJpeg(slot.data, length, true, slot.rtp);
  slot.rtpValid = err == rtpjpeg::JpegError::kOk;
  if (!slot.rtpValid) {
    ++stats_.rtpRejected;
    log_w("frame not RTP/JPEG compatible (%s), %u bytes", rtpjpeg::toString(err), unsigned(length));
  }
  slot.sequence = ++sequence_;
  if (slot.sequence == 0) slot.sequence = ++sequence_;  // 0 means "none"
  return true;
}

void FrameHub::poll(uint64_t nowUs, bool demand) {
  if (!demand || !camera::running()) {
    waitingSinceUs_ = 0;
    return;
  }
  if (nowUs < nextCaptureUs_) return;

  if (!esp_camera_available_frames()) {
    if (waitingSinceUs_ == 0) waitingSinceUs_ = nowUs;
    if (nowUs - waitingSinceUs_ < kMaxWaitForFrameUs) return;
  }
  waitingSinceUs_ = 0;

  FrameSlot *slot = freeSlot();
  if (slot == nullptr) {
    ++stats_.noFreeSlot;  // two laggards hold older frames: try again next loop
    return;
  }
  CameraFrame fb = camera::grab();
  if (!fb || fb->format != PIXFORMAT_JPEG || fb->len == 0) {
    ++stats_.captureFailed;
    nextCaptureUs_ = nowUs + cfg::kFramePeriodUs / 4;
    return;
  }
  // The driver stamps the frame with esp_timer time at VSYNC. RTP timestamps
  // must increase: fall back to "now" if the stamp is missing or not monotonic.
  uint64_t captureUs = uint64_t(fb->timestamp.tv_sec) * 1000000u + uint64_t(fb->timestamp.tv_usec);
  if (captureUs <= lastCaptureUs_ || captureUs > nowUs) captureUs = nowUs;
  lastCaptureUs_ = captureUs;
  const bool ok = store(*slot, fb->buf, fb->len, captureUs);
  fb.release();  // back to the driver right away
  if (!ok) return;

  latest_ = slot;
  ++stats_.captured;
  stats_.lastSize = slot->length;
  // Fixed cadence without bursts: if we fell behind, restart from now.
  nextCaptureUs_ += cfg::kFramePeriodUs;
  if (nextCaptureUs_ + cfg::kFramePeriodUs < nowUs || nextCaptureUs_ > nowUs + cfg::kFramePeriodUs) {
    nextCaptureUs_ = nowUs + cfg::kFramePeriodUs;
  }
}

FrameRef FrameHub::latestAfter(uint32_t afterSequence) {
  if (latest_ == nullptr || latest_->sequence == afterSequence) return FrameRef();
  return FrameRef(latest_);
}

void FrameHub::releaseMemory() {
  for (FrameSlot &s : slots_) {
    if (s.refs != 0) continue;
    if (&s == latest_) latest_ = nullptr;
    heap_caps_free(s.data);
    s = FrameSlot{};
  }
}

FrameHub::Stats FrameHub::takeStats() {
  const Stats s = stats_;
  stats_ = Stats{};
  stats_.lastSize = s.lastSize;
  return s;
}
