// One capture per frame period, shared by every RTSP and HTTP client.
//
// Each captured JPEG is copied into a PSRAM slot (the driver buffer is
// returned immediately, so the sensor never starves) and parsed once for
// RFC 2435. Consumers hold a reference-counted FrameRef while they send a
// frame and ask for the newest one when done: a slow client skips frames
// instead of queueing them, and never slows down the others.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "JpegScan.h"

struct FrameSlot {
  uint8_t *data = nullptr;
  size_t capacity = 0;
  size_t length = 0;
  rtpjpeg::JpegInfo rtp;  // valid only if rtpValid (pointers into data)
  bool rtpValid = false;
  uint64_t captureUs = 0;  // esp_timer time at the start of the exposure
  uint32_t sequence = 0;   // 0 = empty slot
  uint16_t refs = 0;
};

// Move-only handle keeping a slot alive.
class FrameRef {
 public:
  FrameRef() = default;
  explicit FrameRef(FrameSlot *slot) : slot_(slot) {
    if (slot_ != nullptr) ++slot_->refs;
  }
  ~FrameRef() { reset(); }
  FrameRef(FrameRef &&other) noexcept : slot_(other.slot_) { other.slot_ = nullptr; }
  FrameRef &operator=(FrameRef &&other) noexcept {
    if (this != &other) {
      reset();
      slot_ = other.slot_;
      other.slot_ = nullptr;
    }
    return *this;
  }
  FrameRef(const FrameRef &) = delete;
  FrameRef &operator=(const FrameRef &) = delete;

  explicit operator bool() const { return slot_ != nullptr; }
  const FrameSlot *operator->() const { return slot_; }
  const FrameSlot &operator*() const { return *slot_; }
  void reset() {
    if (slot_ != nullptr) {
      --slot_->refs;
      slot_ = nullptr;
    }
  }

 private:
  FrameSlot *slot_ = nullptr;
};

class FrameHub {
 public:
  // Captures a new frame when one is due and at least one consumer wants it.
  void poll(uint64_t nowUs, bool demand);
  // Newest frame with sequence > `afterSequence`, or an empty ref.
  FrameRef latestAfter(uint32_t afterSequence);
  // Drops every slot not referenced (used before OTA to give memory back).
  void releaseMemory();

  struct Stats {
    uint32_t captured = 0;
    uint32_t rtpRejected = 0;   // frames HTTP can serve but RFC 2435 cannot carry
    uint32_t noFreeSlot = 0;
    uint32_t captureFailed = 0;
    size_t lastSize = 0;
  };
  Stats takeStats();

 private:
  // Latest frame + one per concurrently lagging consumer before capture stalls.
  static constexpr size_t kSlots = 4;

  FrameSlot *freeSlot();
  bool store(FrameSlot &slot, const uint8_t *data, size_t length, uint64_t captureUs);

  FrameSlot slots_[kSlots];
  FrameSlot *latest_ = nullptr;
  uint32_t sequence_ = 0;
  uint64_t nextCaptureUs_ = 0;
  uint64_t waitingSinceUs_ = 0;
  uint64_t lastCaptureUs_ = 0;
  Stats stats_;
};
