// Fixed-capacity transmit queue drained into a non-blocking TCP socket.
//
// Data leaves in order: the internal buffer first, then an optional external
// span (zero-copy, e.g. JPEG bytes owned by a FrameHub slot). Partial writes
// resume exactly where they stopped, so a stream is never corrupted by
// re-sending headers (a bug of the previous firmware). A peer that accepts no
// byte for `stallTimeoutMs` while data is pending is reported as failed.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "net/Socket.h"

namespace net {

enum class FlushResult : uint8_t { kDone, kPending, kFailed };

template <size_t Capacity>
class Outbox {
 public:
  bool empty() const { return pos_ == len_ && extLen_ == 0; }
  bool hasExternal() const { return extLen_ != 0; }
  // Bytes that reserve() can still provide (0 while an external span is queued).
  size_t room() const { return extLen_ != 0 ? 0 : Capacity - (len_ - pos_); }

  // Contiguous writable area of `n` bytes, or nullptr if it does not fit.
  // Finish with commit(written).
  uint8_t *reserve(size_t n) {
    if (extLen_ != 0) return nullptr;  // external data must stay last
    if (n > Capacity - len_ && pos_ != 0) {
      memmove(buf_, buf_ + pos_, len_ - pos_);
      len_ -= pos_;
      pos_ = 0;
    }
    return n <= Capacity - len_ ? buf_ + len_ : nullptr;
  }
  void commit(size_t n) { len_ += n; }

  bool append(const void *data, size_t n) {
    uint8_t *p = reserve(n);
    if (p == nullptr) return false;
    memcpy(p, data, n);
    commit(n);
    return true;
  }
  // Queues `data` without copying: it must stay valid until empty().
  bool appendExternal(const uint8_t *data, size_t n) {
    if (extLen_ != 0) return false;
    ext_ = data;
    extLen_ = n;
    return true;
  }
  void clear() {
    pos_ = len_ = extLen_ = 0;
    ext_ = nullptr;
    stalled_ = false;
  }

  FlushResult flush(const Socket &s, uint32_t nowMs, uint32_t stallTimeoutMs) {
    while (!empty()) {
      const bool internal = pos_ < len_;
      const uint8_t *data = internal ? buf_ + pos_ : ext_;
      const size_t n = internal ? len_ - pos_ : extLen_;
      const IoResult r = sendSome(s, data, n);
      if (r.status == IoStatus::kOk) {
        stalled_ = false;
        if (internal) {
          pos_ += r.bytes;
        } else {
          ext_ += r.bytes;
          extLen_ -= r.bytes;
        }
      } else if (r.status == IoStatus::kWouldBlock) {
        if (!stalled_) {
          stalled_ = true;
          stallSinceMs_ = nowMs;
        }
        return nowMs - stallSinceMs_ > stallTimeoutMs ? FlushResult::kFailed : FlushResult::kPending;
      } else {
        return FlushResult::kFailed;
      }
    }
    clear();
    return FlushResult::kDone;
  }

 private:
  uint8_t buf_[Capacity];
  size_t pos_ = 0;
  size_t len_ = 0;
  const uint8_t *ext_ = nullptr;
  size_t extLen_ = 0;
  uint32_t stallSinceMs_ = 0;
  bool stalled_ = false;
};

}  // namespace net
