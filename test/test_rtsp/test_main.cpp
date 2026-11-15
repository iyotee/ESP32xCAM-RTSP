// Host unit tests for lib/rtsp:  pio test -e native -f test_rtsp
#include <unity.h>

#include <cstring>
#include <string>

#include "RtspParser.h"

using namespace rtsp;

void setUp() {}
void tearDown() {}

namespace {

constexpr size_t kCap = 512;

const char kSetup[] =
    "SETUP rtsp://192.168.1.173:8554/stream=0/track1 RTSP/1.0\r\n"
    "CSeq: 3\r\n"
    "User-Agent: Lavf62.3.100\r\n"
    "transport:  RTP/AVP/TCP;unicast;interleaved=0-1 \r\n"
    "\r\n";

std::string sv(std::string_view v) { return std::string(v); }

ParseStatus parse(const std::string &s, Message &m) { return parseMessage(s.data(), s.size(), kCap, m); }

}  // namespace

void test_full_request() {
  Message m;
  TEST_ASSERT_TRUE(parseMessage(kSetup, strlen(kSetup), kCap, m) == ParseStatus::kRequest);
  TEST_ASSERT_EQUAL_STRING("SETUP", sv(m.method).c_str());
  TEST_ASSERT_EQUAL_STRING("rtsp://192.168.1.173:8554/stream=0/track1", sv(m.uri).c_str());
  TEST_ASSERT_EQUAL_STRING("3", sv(m.cseq).c_str());
  TEST_ASSERT_EQUAL_STRING("RTP/AVP/TCP;unicast;interleaved=0-1", sv(m.transport).c_str());
  TEST_ASSERT_EQUAL(strlen(kSetup), m.consumed);
}

void test_fragmented_request_is_incomplete_until_last_byte() {
  Message m;
  for (size_t len = 1; len < strlen(kSetup); ++len) {
    TEST_ASSERT_TRUE(parseMessage(kSetup, len, kCap, m) == ParseStatus::kIncomplete);
  }
  TEST_ASSERT_TRUE(parseMessage(kSetup, strlen(kSetup), kCap, m) == ParseStatus::kRequest);
}

void test_pipelined_requests_and_session_header() {
  const std::string two = std::string("PLAY rtsp://h/stream=0 RTSP/1.0\r\nCSeq: 5\r\n"
                                      "Session: 1A2B3C4D;timeout=60\r\n\r\n") + kSetup;
  Message m;
  TEST_ASSERT_TRUE(parse(two, m) == ParseStatus::kRequest);
  TEST_ASSERT_EQUAL_STRING("1A2B3C4D", sv(m.session).c_str());
  const std::string rest = two.substr(m.consumed);
  TEST_ASSERT_TRUE(parse(rest, m) == ParseStatus::kRequest);
  TEST_ASSERT_EQUAL_STRING("SETUP", sv(m.method).c_str());
}

void test_body_is_waited_for_and_consumed() {
  const std::string part = "SET_PARAMETER rtsp://h/stream=0 RTSP/1.0\r\nCSeq: 9\r\nContent-Length: 5\r\n\r\nab";
  Message m;
  TEST_ASSERT_TRUE(parse(part, m) == ParseStatus::kIncomplete);
  const std::string full = part + "cde";
  TEST_ASSERT_TRUE(parse(full, m) == ParseStatus::kRequest);
  TEST_ASSERT_EQUAL(full.size(), m.consumed);
}

void test_interleaved_frames() {
  const char rtcp[] = {'$', 1, 0, 3, 'x', 'y', 'z', 'O'};
  Message m;
  TEST_ASSERT_TRUE(parseMessage(rtcp, 3, kCap, m) == ParseStatus::kIncomplete);
  TEST_ASSERT_TRUE(parseMessage(rtcp, sizeof(rtcp), kCap, m) == ParseStatus::kInterleaved);
  TEST_ASSERT_EQUAL_UINT8(1, m.channel);
  TEST_ASSERT_EQUAL(7u, m.consumed);
  TEST_ASSERT_EQUAL(0u, m.skipAfter);
  // Partial frame: everything available is consumed, the rest must be skipped.
  TEST_ASSERT_TRUE(parseMessage(rtcp, 5, kCap, m) == ParseStatus::kInterleaved);
  TEST_ASSERT_EQUAL(5u, m.consumed);
  TEST_ASSERT_EQUAL(2u, m.skipAfter);
}

void test_stray_crlf_ignored() {
  Message m;
  TEST_ASSERT_TRUE(parseMessage("\r\n\r\nOPT", 7, kCap, m) == ParseStatus::kIgnored);
  TEST_ASSERT_EQUAL(4u, m.consumed);
}

void test_invalid_and_too_large() {
  Message m;
  TEST_ASSERT_TRUE(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", m) == ParseStatus::kInvalid);
  TEST_ASSERT_TRUE(parse("OPTIONS * RTSP/1.0\r\nbroken header\r\n\r\n", m) == ParseStatus::kInvalid);
  TEST_ASSERT_TRUE(parse(std::string(kCap, 'A'), m) == ParseStatus::kTooLarge);
  TEST_ASSERT_TRUE(parse("ANNOUNCE * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 999999\r\n\r\n", m) ==
                   ParseStatus::kTooLarge);
  TEST_ASSERT_TRUE(parse("ANNOUNCE * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 600\r\n\r\n", m) ==
                   ParseStatus::kTooLarge);
}

void test_transport_header() {
  Transport t;
  TEST_ASSERT_TRUE(parseTransport("RTP/AVP/TCP;unicast;interleaved=2-3", t));
  TEST_ASSERT_TRUE(t.tcp);
  TEST_ASSERT_EQUAL_UINT8(2, t.rtpChannel);
  TEST_ASSERT_EQUAL_UINT8(3, t.rtcpChannel);
  TEST_ASSERT_TRUE(parseTransport("RTP/AVP/TCP;unicast", t));
  TEST_ASSERT_EQUAL_UINT8(0, t.rtpChannel);
  TEST_ASSERT_TRUE(parseTransport("RTP/AVP;unicast;client_port=50000-50001", t));
  TEST_ASSERT_FALSE(t.tcp);
  TEST_ASSERT_EQUAL_UINT16(50000, t.clientRtpPort);
  TEST_ASSERT_EQUAL_UINT16(50001, t.clientRtcpPort);
  // First spec unusable (multicast), second accepted.
  TEST_ASSERT_TRUE(parseTransport("RTP/AVP;multicast, RTP/AVP/UDP;unicast;client_port=6000", t));
  TEST_ASSERT_EQUAL_UINT16(6000, t.clientRtpPort);
  TEST_ASSERT_EQUAL_UINT16(6001, t.clientRtcpPort);
  TEST_ASSERT_FALSE(parseTransport("RTP/AVP;unicast", t));  // UDP without ports
  TEST_ASSERT_FALSE(parseTransport("RTP/AVP;unicast;client_port=70000-70001", t));
  TEST_ASSERT_FALSE(parseTransport("RAW/RAW/UDP;unicast;client_port=1-2", t));
  TEST_ASSERT_FALSE(parseTransport("RTP/AVP/TCP;interleaved=300-301", t));
}

void test_uri_classification() {
  auto cls = [](const char *uri) { return classifyUri(uri, "stream=0", "track1"); };
  TEST_ASSERT_TRUE(cls("rtsp://1.2.3.4:8554/stream=0") == UriTarget::kStream);
  TEST_ASSERT_TRUE(cls("rtsp://1.2.3.4:8554/stream=0/") == UriTarget::kStream);
  TEST_ASSERT_TRUE(cls("rtsp://1.2.3.4/stream=0/track1") == UriTarget::kTrack);
  TEST_ASSERT_TRUE(cls("/stream=0") == UriTarget::kStream);
  TEST_ASSERT_TRUE(cls("*") == UriTarget::kStream);
  TEST_ASSERT_TRUE(cls("rtsp://1.2.3.4/stream=01") == UriTarget::kNone);
  TEST_ASSERT_TRUE(cls("rtsp://1.2.3.4/stream=0/track2") == UriTarget::kNone);
  TEST_ASSERT_TRUE(cls("rtsp://1.2.3.4") == UriTarget::kNone);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_full_request);
  RUN_TEST(test_fragmented_request_is_incomplete_until_last_byte);
  RUN_TEST(test_pipelined_requests_and_session_header);
  RUN_TEST(test_body_is_waited_for_and_consumed);
  RUN_TEST(test_interleaved_frames);
  RUN_TEST(test_stray_crlf_ignored);
  RUN_TEST(test_invalid_and_too_large);
  RUN_TEST(test_transport_header);
  RUN_TEST(test_uri_classification);
  return UNITY_END();
}

