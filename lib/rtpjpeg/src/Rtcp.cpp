#include "Rtcp.h"

#include <cstring>

namespace rtpjpeg {
namespace {

constexpr uint8_t kPtSenderReport = 200;
constexpr uint8_t kPtSdes = 202;
constexpr uint8_t kSdesCname = 1;
constexpr size_t kMaxCname = 64;

uint8_t *put16(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
  return p + 2;
}

uint8_t *put32(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
  return p + 4;
}

}  // namespace

size_t buildSenderReport(uint8_t *out, size_t capacity, const SenderReport &sr, const char *cname) {
  size_t cnameLen = cname == nullptr ? 0 : std::strlen(cname);
  if (cnameLen > kMaxCname) cnameLen = kMaxCname;
  // SDES chunk: SSRC + CNAME item (type, length, text) + END, padded to 32 bits.
  const size_t chunk = (4 + 2 + cnameLen + 1 + 3) & ~size_t(3);
  const size_t total = 28 + 4 + chunk;
  if (out == nullptr || capacity < total) return 0;

  uint8_t *p = out;
  *p++ = 0x80;  // V=2, P=0, RC=0
  *p++ = kPtSenderReport;
  p = put16(p, 6);  // length in 32-bit words minus one
  p = put32(p, sr.ssrc);
  const uint64_t seconds = sr.ntpMicros / 1000000u;
  const uint64_t micros = sr.ntpMicros % 1000000u;
  p = put32(p, uint32_t(seconds));
  p = put32(p, uint32_t((micros << 32) / 1000000u));
  p = put32(p, sr.rtpTimestamp);
  p = put32(p, sr.packetCount);
  p = put32(p, sr.octetCount);

  *p++ = 0x81;  // V=2, P=0, SC=1
  *p++ = kPtSdes;
  p = put16(p, uint16_t((4 + chunk) / 4 - 1));
  p = put32(p, sr.ssrc);
  *p++ = kSdesCname;
  *p++ = uint8_t(cnameLen);
  std::memcpy(p, cname, cnameLen);
  p += cnameLen;
  // END item + padding: all zero bytes up to the chunk boundary.
  const size_t pad = total - size_t(p - out);
  std::memset(p, 0, pad);
  return total;
}

}  // namespace rtpjpeg
