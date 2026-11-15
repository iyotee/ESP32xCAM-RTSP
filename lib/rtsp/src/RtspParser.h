// Incremental, bounded RTSP/1.0 (RFC 2326) request parser. Pure C++, no
// allocation: results are string_views into the caller's receive buffer and
// stay valid until the caller drops `consumed` bytes from that buffer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rtsp {

enum class ParseStatus : uint8_t {
  kIncomplete,   // need more bytes (nothing consumed)
  kRequest,      // a full request (headers + body) is available
  kInterleaved,  // a "$" framed RTP/RTCP packet from the client (RFC 2326 10.12)
  kIgnored,      // stray CR/LF between messages
  kInvalid,      // not RTSP: answer 400 and close (the stream cannot be resynced)
  kTooLarge,     // request does not fit in the receive buffer
};

struct Message {
  // kRequest
  std::string_view method;
  std::string_view uri;
  std::string_view cseq;
  std::string_view session;  // without ";timeout=..." parameters
  std::string_view transport;
  size_t contentLength = 0;
  // kInterleaved
  uint8_t channel = 0;
  size_t skipAfter = 0;  // bytes of the frame not yet received: discard them as they arrive
  // Every status except kIncomplete/kInvalid/kTooLarge: bytes to drop from the buffer front.
  size_t consumed = 0;
};

// `capacity` is the size of the caller's buffer: a request that cannot be
// complete within it yields kTooLarge instead of kIncomplete forever.
ParseStatus parseMessage(const char *buf, size_t len, size_t capacity, Message &msg);

struct Transport {
  bool tcp = false;
  uint16_t clientRtpPort = 0;   // UDP only
  uint16_t clientRtcpPort = 0;  // UDP only
  uint8_t rtpChannel = 0;       // TCP only
  uint8_t rtcpChannel = 1;      // TCP only
};

// Picks the first acceptable unicast spec of a Transport header: RTP/AVP[/UDP]
// with client_port, or RTP/AVP/TCP (interleaved defaults to 0-1).
bool parseTransport(std::string_view header, Transport &out);

enum class UriTarget : uint8_t { kNone, kStream, kTrack };

// Classifies an absolute RTSP URI against the served stream path (e.g.
// "stream=0") and its single track control name (e.g. "track1").
UriTarget classifyUri(std::string_view uri, std::string_view streamPath, std::string_view track);

bool equalsIgnoreCase(std::string_view a, std::string_view b);

}  // namespace rtsp
