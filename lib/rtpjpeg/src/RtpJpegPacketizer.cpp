#include "RtpJpegPacketizer.h"

#include <cstring>

namespace rtpjpeg {
namespace {

inline uint8_t *put16(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
  return p + 2;
}

inline uint8_t *put32(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
  return p + 4;
}

}  // namespace

RtpJpegPacketizer::RtpJpegPacketizer(uint32_t ssrc, uint16_t initialSequence, size_t maxPayload)
    : ssrc_(ssrc),
      sequence_(initialSequence),
      maxPayload_(maxPayload < kMinPayloadBudget ? kMinPayloadBudget : maxPayload) {}

void RtpJpegPacketizer::begin(const JpegInfo &info, uint32_t rtpTimestamp) {
  info_ = &info;
  offset_ = 0;
  timestamp_ = rtpTimestamp;
}

size_t RtpJpegPacketizer::next(uint8_t *out, size_t capacity) {
  if (!hasNext() || out == nullptr || capacity < maxPacketSize()) return 0;
  const JpegInfo &info = *info_;
  const bool first = offset_ == 0;

  size_t headers = kJpegHeaderSize;
  if (info.restartInterval != 0) headers += kRestartHeaderSize;
  if (first) headers += kQuantHeaderSize;

  const size_t room = maxPayload_ - headers;
  const size_t remaining = info.scanLength - offset_;
  const size_t chunk = remaining < room ? remaining : room;
  const bool last = chunk == remaining;

  uint8_t *p = out;
  // RTP fixed header (RFC 3550 5.1): V=2, P=0, X=0, CC=0.
  *p++ = 0x80;
  *p++ = uint8_t((last ? 0x80 : 0x00) | kPayloadTypeJpeg);  // M bit = last packet of the frame
  p = put16(p, sequence_++);
  p = put32(p, timestamp_);
  p = put32(p, ssrc_);

  // JPEG header (RFC 2435 3.1): type-specific, 24-bit fragment offset, type,
  // Q, width/8, height/8.
  *p++ = 0;
  *p++ = uint8_t(offset_ >> 16);
  *p++ = uint8_t(offset_ >> 8);
  *p++ = uint8_t(offset_);
  *p++ = info.rtpType();
  *p++ = 255;
  *p++ = uint8_t(info.width / 8);
  *p++ = uint8_t(info.height / 8);

  if (info.restartInterval != 0) {
    // RFC 2435 3.1.7: packets are not aligned on restart intervals, so F=L=1
    // and restart count = 0x3FFF.
    p = put16(p, info.restartInterval);
    p = put16(p, 0xFFFF);
  }

  if (first) {
    // RFC 2435 3.1.8: MBZ, precision (0 = 8-bit for both tables), length.
    *p++ = 0;
    *p++ = 0;
    p = put16(p, 128);
    std::memcpy(p, info.lumaQuant, 64);
    std::memcpy(p + 64, info.chromaQuant, 64);
    p += 128;
  }

  std::memcpy(p, info.scan + offset_, chunk);
  p += chunk;
  offset_ += chunk;
  return size_t(p - out);
}

}  // namespace rtpjpeg
