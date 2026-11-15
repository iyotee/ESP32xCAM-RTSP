#include "RtspParser.h"

namespace rtsp {
namespace {

constexpr size_t kMaxContentLength = 4096;

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool startsWithIgnoreCase(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && equalsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

// Decimal integer in [0, maxValue]; rejects empty strings and overflow.
bool parseUint(std::string_view s, uint32_t maxValue, uint32_t &out) {
  if (s.empty()) return false;
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const uint32_t digit = uint32_t(c - '0');
    if (v > (maxValue - digit) / 10) return false;
    v = v * 10 + digit;
  }
  out = v;
  return true;
}

// "a-b", or "a" alone (then b = a + 1).
bool parsePair(std::string_view s, uint32_t maxValue, uint32_t &a, uint32_t &b) {
  const size_t dash = s.find('-');
  if (dash == std::string_view::npos) {
    if (!parseUint(s, maxValue - 1, a)) return false;
    b = a + 1;
    return true;
  }
  return parseUint(s.substr(0, dash), maxValue, a) && parseUint(s.substr(dash + 1), maxValue, b);
}

bool isMethodToken(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!((c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
  }
  return true;
}

// Splits `s` at the first `sep`: returns the head, leaves the tail in `s`.
std::string_view popToken(std::string_view &s, char sep) {
  const size_t at = s.find(sep);
  const std::string_view head = s.substr(0, at);
  s = at == std::string_view::npos ? std::string_view() : s.substr(at + 1);
  return trim(head);
}

}  // namespace

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

ParseStatus parseMessage(const char *buf, size_t len, size_t capacity, Message &msg) {
  msg = Message{};
  if (len == 0) return ParseStatus::kIncomplete;

  if (buf[0] == '\r' || buf[0] == '\n') {
    size_t n = 0;
    while (n < len && (buf[n] == '\r' || buf[n] == '\n')) ++n;
    msg.consumed = n;
    return ParseStatus::kIgnored;
  }

  if (buf[0] == '$') {
    if (len < 4) return ParseStatus::kIncomplete;
    const size_t total = 4 + ((size_t(uint8_t(buf[2])) << 8) | uint8_t(buf[3]));
    msg.channel = uint8_t(buf[1]);
    // Never wait for a whole frame (it may exceed the buffer): drop what is
    // there and let the caller discard the remainder as it arrives.
    msg.consumed = total <= len ? total : len;
    msg.skipAfter = total - msg.consumed;
    return ParseStatus::kInterleaved;
  }

  const std::string_view all(buf, len);
  const size_t headerEnd = all.find("\r\n\r\n");
  if (headerEnd == std::string_view::npos) {
    return len >= capacity ? ParseStatus::kTooLarge : ParseStatus::kIncomplete;
  }

  // Request line: METHOD SP Request-URI SP RTSP-Version
  std::string_view head = all.substr(0, headerEnd + 2);  // keep the last CRLF as terminator
  const size_t lineEnd = head.find("\r\n");
  std::string_view line = head.substr(0, lineEnd);
  head.remove_prefix(lineEnd + 2);
  msg.method = popToken(line, ' ');
  msg.uri = popToken(line, ' ');
  if (!isMethodToken(msg.method) || msg.uri.empty() || !startsWithIgnoreCase(line, "RTSP/1.")) {
    return ParseStatus::kInvalid;
  }

  while (!head.empty()) {
    const size_t eol = head.find("\r\n");
    std::string_view header = head.substr(0, eol);
    head.remove_prefix(eol + 2);
    const size_t colon = header.find(':');
    if (colon == std::string_view::npos) return ParseStatus::kInvalid;
    const std::string_view name = trim(header.substr(0, colon));
    const std::string_view value = trim(header.substr(colon + 1));
    if (equalsIgnoreCase(name, "CSeq")) {
      msg.cseq = value;
    } else if (equalsIgnoreCase(name, "Session")) {
      msg.session = trim(value.substr(0, value.find(';')));
    } else if (equalsIgnoreCase(name, "Transport")) {
      msg.transport = value;
    } else if (equalsIgnoreCase(name, "Content-Length")) {
      uint32_t n = 0;
      if (!parseUint(value, kMaxContentLength, n)) return ParseStatus::kTooLarge;
      msg.contentLength = n;
    }
  }

  const size_t total = headerEnd + 4 + msg.contentLength;
  if (total > capacity) return ParseStatus::kTooLarge;
  if (total > len) return ParseStatus::kIncomplete;
  msg.consumed = total;
  return ParseStatus::kRequest;
}

bool parseTransport(std::string_view header, Transport &out) {
  while (!header.empty()) {
    std::string_view spec = popToken(header, ',');
    Transport t;
    bool valid = true, haveClientPort = false, multicast = false;
    const std::string_view profile = popToken(spec, ';');
    if (equalsIgnoreCase(profile, "RTP/AVP/TCP")) {
      t.tcp = true;
    } else if (!equalsIgnoreCase(profile, "RTP/AVP") && !equalsIgnoreCase(profile, "RTP/AVP/UDP")) {
      valid = false;
    }
    while (valid && !spec.empty()) {
      const std::string_view param = popToken(spec, ';');
      uint32_t a = 0, b = 0;
      if (equalsIgnoreCase(param, "multicast")) {
        multicast = true;
      } else if (startsWithIgnoreCase(param, "client_port=")) {
        valid = parsePair(param.substr(12), 65535, a, b) && a != 0 && b != 0;
        haveClientPort = valid;
        t.clientRtpPort = uint16_t(a);
        t.clientRtcpPort = uint16_t(b);
      } else if (startsWithIgnoreCase(param, "interleaved=")) {
        valid = parsePair(param.substr(12), 255, a, b);
        t.rtpChannel = uint8_t(a);
        t.rtcpChannel = uint8_t(b);
      }
    }
    if (valid && !multicast && (t.tcp || haveClientPort)) {
      out = t;
      return true;
    }
  }
  return false;
}

UriTarget classifyUri(std::string_view uri, std::string_view streamPath, std::string_view track) {
  if (uri == "*") return UriTarget::kStream;
  // Strip "rtsp://host[:port]"; a bare absolute path is accepted too.
  if (startsWithIgnoreCase(uri, "rtsp://")) {
    const size_t slash = uri.find('/', 7);
    uri = slash == std::string_view::npos ? std::string_view() : uri.substr(slash);
  }
  while (!uri.empty() && uri.front() == '/') uri.remove_prefix(1);
  while (!uri.empty() && uri.back() == '/') uri.remove_suffix(1);
  if (uri.substr(0, streamPath.size()) != streamPath) return UriTarget::kNone;
  std::string_view rest = uri.substr(streamPath.size());
  if (rest.empty()) return UriTarget::kStream;
  if (rest.front() != '/') return UriTarget::kNone;
  rest.remove_prefix(1);
  return rest == track ? UriTarget::kTrack : UriTarget::kNone;
}

}  // namespace rtsp

