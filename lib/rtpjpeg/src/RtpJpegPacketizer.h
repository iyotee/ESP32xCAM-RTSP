// RFC 3550 / RFC 2435 packetizer: turns one parsed JPEG frame into a sequence
// of RTP packets. Pure C++, no allocation.
//
// Packet layout produced by next():
//   RTP header (12) | JPEG header (8) | [Restart header (4)] |
//   [Quantization table header (4) + 128 bytes, first packet only] | scan data
//
// Q is always 255: the quantization tables are sent in-band, so the receiver
// never has to guess them from a quality factor.
#pragma once

#include <cstddef>
#include <cstdint>

#include "JpegScan.h"

namespace rtpjpeg {

constexpr uint8_t kPayloadTypeJpeg = 26;  // RFC 3551 static payload type
constexpr uint32_t kClockRateHz = 90000;
constexpr size_t kRtpHeaderSize = 12;
constexpr size_t kJpegHeaderSize = 8;
constexpr size_t kRestartHeaderSize = 4;
constexpr size_t kQuantHeaderSize = 4 + 2 * 64;
// Smallest payload budget that still leaves room for scan data in packet 1.
constexpr size_t kMinPayloadBudget = kJpegHeaderSize + kRestartHeaderSize + kQuantHeaderSize + 64;

class RtpJpegPacketizer {
 public:
  RtpJpegPacketizer() = default;
  // `maxPayload` = bytes allowed after the 12-byte RTP header.
  RtpJpegPacketizer(uint32_t ssrc, uint16_t initialSequence, size_t maxPayload);

  // Starts packetizing `info` (must stay valid until hasNext() is false).
  void begin(const JpegInfo &info, uint32_t rtpTimestamp);
  // Drops the rest of the current frame (the receiver discards it as incomplete).
  void abort() { info_ = nullptr; }
  bool hasNext() const { return info_ != nullptr && offset_ < info_->scanLength; }

  // Writes the next packet at `out`. Returns its size, or 0 if `capacity` is
  // smaller than maxPacketSize() or no packet is pending.
  size_t next(uint8_t *out, size_t capacity);

  size_t maxPacketSize() const { return kRtpHeaderSize + maxPayload_; }
  uint32_t ssrc() const { return ssrc_; }
  uint16_t nextSequence() const { return sequence_; }
  uint32_t lastTimestamp() const { return timestamp_; }

 private:
  const JpegInfo *info_ = nullptr;
  size_t offset_ = 0;
  uint32_t timestamp_ = 0;
  uint32_t ssrc_ = 0;
  uint16_t sequence_ = 0;
  size_t maxPayload_ = 1400;
};

}  // namespace rtpjpeg
