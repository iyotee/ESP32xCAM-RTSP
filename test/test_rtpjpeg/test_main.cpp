// Host unit tests for lib/rtpjpeg:  pio test -e native -f test_rtpjpeg
#include <unity.h>

#include <cstring>
#include <vector>

#include "JpegScan.h"
#include "Rtcp.h"
#include "RtpJpegPacketizer.h"
#include "fixtures.h"

using namespace rtpjpeg;

void setUp() {}
void tearDown() {}

namespace {

uint16_t be16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Packetizes `info`, checks every RFC 2435 invariant, returns the reassembled scan.
std::vector<uint8_t> packetizeAndCheck(const JpegInfo &info, size_t maxPayload, size_t *packets) {
  RtpJpegPacketizer pk(0x11223344u, 0xFFFE, maxPayload);  // sequence wraps on purpose
  std::vector<uint8_t> buf(pk.maxPacketSize());
  std::vector<uint8_t> scan;
  pk.begin(info, 0xABCDEF01u);
  uint16_t expectedSeq = 0xFFFE;
  size_t count = 0;
  while (pk.hasNext()) {
    const size_t n = pk.next(buf.data(), buf.size());
    TEST_ASSERT_TRUE(n > kRtpHeaderSize + kJpegHeaderSize);
    TEST_ASSERT_TRUE(n <= pk.maxPacketSize());
    const uint8_t *p = buf.data();
    TEST_ASSERT_EQUAL_HEX8(0x80, p[0]);
    TEST_ASSERT_EQUAL_UINT8(kPayloadTypeJpeg, p[1] & 0x7F);
    TEST_ASSERT_EQUAL_UINT16(expectedSeq, be16(p + 2));
    ++expectedSeq;
    TEST_ASSERT_EQUAL_HEX32(0xABCDEF01u, be32(p + 4));
    TEST_ASSERT_EQUAL_HEX32(0x11223344u, be32(p + 8));
    const bool marker = (p[1] & 0x80) != 0;

    const uint8_t *j = p + kRtpHeaderSize;
    const uint32_t offset = (uint32_t(j[1]) << 16) | (uint32_t(j[2]) << 8) | j[3];
    TEST_ASSERT_EQUAL_UINT8(0, j[0]);
    TEST_ASSERT_EQUAL_UINT32(scan.size(), offset);
    TEST_ASSERT_EQUAL_UINT8(info.rtpType(), j[4]);
    TEST_ASSERT_EQUAL_UINT8(255, j[5]);
    TEST_ASSERT_EQUAL_UINT8(info.width / 8, j[6]);
    TEST_ASSERT_EQUAL_UINT8(info.height / 8, j[7]);
    const uint8_t *q = j + kJpegHeaderSize;
    if (info.restartInterval != 0) {
      TEST_ASSERT_EQUAL_UINT16(info.restartInterval, be16(q));
      TEST_ASSERT_EQUAL_HEX16(0xFFFF, be16(q + 2));
      q += kRestartHeaderSize;
    }
    if (offset == 0) {
      TEST_ASSERT_EQUAL_UINT8(0, q[0]);
      TEST_ASSERT_EQUAL_UINT8(0, q[1]);
      TEST_ASSERT_EQUAL_UINT16(128, be16(q + 2));
      TEST_ASSERT_EQUAL_MEMORY(info.lumaQuant, q + 4, 64);
      TEST_ASSERT_EQUAL_MEMORY(info.chromaQuant, q + 68, 64);
      q += kQuantHeaderSize;
    }
    const uint8_t *packetEnd = buf.data() + n;
    scan.insert(scan.end(), q, packetEnd);
    TEST_ASSERT_EQUAL(!pk.hasNext(), marker);
    ++count;
  }
  TEST_ASSERT_EQUAL_UINT16(expectedSeq, pk.nextSequence());
  TEST_ASSERT_EQUAL(0u, pk.next(buf.data(), buf.size()));  // nothing pending
  if (packets) *packets = count;
  return scan;
}

void expectParses(const uint8_t *data, size_t len, uint8_t type, uint16_t dri) {
  JpegInfo info;
  TEST_ASSERT_EQUAL_STRING("ok", toString(parseJpeg(data, len, false, info)));
  TEST_ASSERT_EQUAL_UINT16(64, info.width);
  TEST_ASSERT_EQUAL_UINT16(48, info.height);
  TEST_ASSERT_EQUAL_UINT8(type, info.rtpType());
  TEST_ASSERT_EQUAL_UINT16(dri, info.restartInterval);
  // The scan carries no header: it starts after SOS and ends right before EOI.
  TEST_ASSERT_TRUE(info.scan > data && info.scan + info.scanLength + 2 == data + len);
  TEST_ASSERT_FALSE(info.scan[0] == 0xFF && info.scan[1] == 0xD8);
  TEST_ASSERT_EQUAL_HEX8(0xFF, info.scan[info.scanLength]);
  TEST_ASSERT_EQUAL_HEX8(0xD9, info.scan[info.scanLength + 1]);
}

void expectError(const uint8_t *data, size_t len, JpegError expected) {
  JpegInfo info;
  TEST_ASSERT_EQUAL_STRING(toString(expected), toString(parseJpeg(data, len, false, info)));
}

}  // namespace

void test_parse_420() { expectParses(kJpeg420, sizeof(kJpeg420), 1, 0); }
void test_parse_422() { expectParses(kJpeg422, sizeof(kJpeg422), 0, 0); }
void test_parse_420_restart_markers() { expectParses(kJpeg420Dri, sizeof(kJpeg420Dri), 65, 2); }

void test_keep_eoi_appends_marker() {
  JpegInfo a, b;
  TEST_ASSERT_TRUE(parseJpeg(kJpeg420, sizeof(kJpeg420), false, a) == JpegError::kOk);
  TEST_ASSERT_TRUE(parseJpeg(kJpeg420, sizeof(kJpeg420), true, b) == JpegError::kOk);
  TEST_ASSERT_EQUAL(a.scanLength + 2, b.scanLength);
  TEST_ASSERT_EQUAL_HEX8(0xD9, b.scan[b.scanLength - 1]);
}

void test_rejects_unsupported_images() {
  expectError(kJpeg444, sizeof(kJpeg444), JpegError::kUnsupportedSampling);
  expectError(kJpegProgressive, sizeof(kJpegProgressive), JpegError::kUnsupportedProcess);
  expectError(kJpegOptimizedHuffman, sizeof(kJpegOptimizedHuffman), JpegError::kNonStandardHuffman);
  expectError(kJpegGray, sizeof(kJpegGray), JpegError::kUnsupportedSampling);
  expectError(kJpegOddSize, sizeof(kJpegOddSize), JpegError::kBadDimensions);
}

void test_rejects_malformed_input() {
  expectError(nullptr, 0, JpegError::kTooShort);
  expectError(kJpeg420, 3, JpegError::kTooShort);
  expectError(kJpeg420 + 1, sizeof(kJpeg420) - 1, JpegError::kNoSoi);
  // Every truncation point must be rejected without reading out of bounds.
  for (size_t len = 4; len < sizeof(kJpeg420) - 1; ++len) {
    std::vector<uint8_t> copy(kJpeg420, kJpeg420 + len);
    JpegInfo info;
    TEST_ASSERT_TRUE(parseJpeg(copy.data(), copy.size(), false, info) != JpegError::kOk);
  }
}

void test_packetize_single_and_multi_packet() {
  JpegInfo info;
  TEST_ASSERT_TRUE(parseJpeg(kJpeg420, sizeof(kJpeg420), false, info) == JpegError::kOk);
  size_t packets = 0;
  std::vector<uint8_t> scan = packetizeAndCheck(info, 4096, &packets);
  TEST_ASSERT_EQUAL(1u, packets);
  TEST_ASSERT_EQUAL_MEMORY(info.scan, scan.data(), info.scanLength);
  scan = packetizeAndCheck(info, kMinPayloadBudget, &packets);
  TEST_ASSERT_TRUE(packets > 3);
  TEST_ASSERT_EQUAL(info.scanLength, scan.size());
  TEST_ASSERT_EQUAL_MEMORY(info.scan, scan.data(), info.scanLength);
}

void test_packetize_restart_markers() {
  JpegInfo info;
  TEST_ASSERT_TRUE(parseJpeg(kJpeg420Dri, sizeof(kJpeg420Dri), true, info) == JpegError::kOk);
  size_t packets = 0;
  std::vector<uint8_t> scan = packetizeAndCheck(info, 300, &packets);
  TEST_ASSERT_TRUE(packets > 1);
  TEST_ASSERT_EQUAL_MEMORY(info.scan, scan.data(), info.scanLength);
}

void test_packetizer_refuses_small_buffer() {
  JpegInfo info;
  TEST_ASSERT_TRUE(parseJpeg(kJpeg422, sizeof(kJpeg422), false, info) == JpegError::kOk);
  RtpJpegPacketizer pk(1, 0, 1400);
  uint8_t small[64];
  pk.begin(info, 0);
  TEST_ASSERT_EQUAL(0u, pk.next(small, sizeof(small)));
  TEST_ASSERT_TRUE(pk.hasNext());
  TEST_ASSERT_EQUAL_UINT16(0, pk.nextSequence());
}

void test_rtcp_sender_report() {
  SenderReport sr;
  sr.ssrc = 0xCAFEBABEu;
  sr.ntpMicros = 12500000ull;  // 12.5 s
  sr.rtpTimestamp = 0x01020304u;
  sr.packetCount = 7;
  sr.octetCount = 9000;
  uint8_t buf[kMaxRtcpSize];
  const size_t n = buildSenderReport(buf, sizeof(buf), sr, "cam@10.0.0.2");  // 12 chars
  TEST_ASSERT_EQUAL(28u + 4u + 20u, n);
  TEST_ASSERT_EQUAL(0u, n % 4);
  TEST_ASSERT_EQUAL_HEX8(0x80, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(200, buf[1]);
  TEST_ASSERT_EQUAL_UINT16(6, be16(buf + 2));
  TEST_ASSERT_EQUAL_HEX32(0xCAFEBABEu, be32(buf + 4));
  TEST_ASSERT_EQUAL_UINT32(12, be32(buf + 8));
  TEST_ASSERT_EQUAL_HEX32(0x80000000u, be32(buf + 12));  // .5 s
  TEST_ASSERT_EQUAL_HEX32(0x01020304u, be32(buf + 16));
  TEST_ASSERT_EQUAL_UINT32(7, be32(buf + 20));
  TEST_ASSERT_EQUAL_UINT32(9000, be32(buf + 24));
  const uint8_t *s = buf + 28;
  TEST_ASSERT_EQUAL_HEX8(0x81, s[0]);
  TEST_ASSERT_EQUAL_UINT8(202, s[1]);
  TEST_ASSERT_EQUAL_UINT16((n - 28) / 4 - 1, be16(s + 2));
  TEST_ASSERT_EQUAL_HEX32(0xCAFEBABEu, be32(s + 4));
  TEST_ASSERT_EQUAL_UINT8(1, s[8]);
  TEST_ASSERT_EQUAL_UINT8(12, s[9]);
  TEST_ASSERT_EQUAL_MEMORY("cam@10.0.0.2", s + 10, 12);
  for (size_t i = 28 + 10 + 12; i < n; ++i) TEST_ASSERT_EQUAL_UINT8(0, buf[i]);
  TEST_ASSERT_EQUAL(0u, buildSenderReport(buf, n - 1, sr, "cam@10.0.0.2"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_rtcp_sender_report);
  RUN_TEST(test_parse_420);
  RUN_TEST(test_parse_422);
  RUN_TEST(test_parse_420_restart_markers);
  RUN_TEST(test_keep_eoi_appends_marker);
  RUN_TEST(test_rejects_unsupported_images);
  RUN_TEST(test_rejects_malformed_input);
  RUN_TEST(test_packetize_single_and_multi_packet);
  RUN_TEST(test_packetize_restart_markers);
  RUN_TEST(test_packetizer_refuses_small_buffer);
  return UNITY_END();
}
