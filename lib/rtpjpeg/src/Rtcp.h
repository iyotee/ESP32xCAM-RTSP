// RTCP Sender Report (RFC 3550 6.4.1) + SDES CNAME (6.5), as the compound
// packet a sender is expected to emit periodically. Pure C++, no allocation.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rtpjpeg {

struct SenderReport {
  uint32_t ssrc = 0;
  // Elapsed time since boot: RFC 3550 6.4.1 allows a sender without
  // wall-clock to use elapsed time as the NTP timestamp.
  uint64_t ntpMicros = 0;
  uint32_t rtpTimestamp = 0;  // same instant as ntpMicros, in the RTP clock
  uint32_t packetCount = 0;
  uint32_t octetCount = 0;  // RTP payload bytes (headers excluded)
};

// Largest packet buildSenderReport() can produce (CNAME is capped at 64 bytes).
constexpr size_t kMaxRtcpSize = 28 + 8 + 2 + 64 + 4;

// Writes SR + SDES(CNAME) at `out`. Returns its size, or 0 if `capacity` is
// too small. A CNAME longer than 64 bytes is truncated.
size_t buildSenderReport(uint8_t *out, size_t capacity, const SenderReport &sr, const char *cname);

}  // namespace rtpjpeg
