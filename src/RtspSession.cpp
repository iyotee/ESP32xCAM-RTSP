#include "RtspSession.h"

#include <esp32-hal-log.h>
#include <esp_random.h>
#include <stdio.h>
#include <string.h>

#include "Rtcp.h"
#include "config.h"

namespace {

constexpr const char *kTrack = "track1";
constexpr size_t kMaxUri = 200;
constexpr size_t kMaxSdp = 512;
constexpr size_t kMaxRtspPacketsPerPoll = 6;  // fairness between sessions (UDP)

int svLen(std::string_view v, size_t max) { return int(v.size() < max ? v.size() : max); }

}  // namespace

void RtspSession::open(net::Socket sock, const sockaddr_in &peer, uint32_t nowMs) {
  sock_ = static_cast<net::Socket &&>(sock);
  peer_ = peer;
  rxLen_ = 0;
  skip_ = 0;
  out_.clear();
  closeAfterFlush_ = false;
  lastActivityMs_ = nowMs;
  state_ = State::kInit;
  sessionId_[0] = '\0';
  dropFrame();
  packetCount_ = 0;
  char ip[16];
  net::formatIp(peer_.sin_addr.s_addr, ip, sizeof(ip));
  log_i("RTSP client %s:%u connected", ip, ntohs(peer_.sin_port));
}

void RtspSession::close(const char *reason) {
  if (!sock_.valid()) return;
  char ip[16];
  net::formatIp(peer_.sin_addr.s_addr, ip, sizeof(ip));
  log_i("RTSP client %s closed: %s (%u RTP packets sent)", ip, reason, unsigned(packetCount_));
  sock_.reset();
  dropFrame();
  out_.clear();
  state_ = State::kInit;
}

bool RtspSession::matchesUdpPeer(const sockaddr_in &from) const {
  return active() && state_ != State::kInit && !transport_.tcp &&
         from.sin_addr.s_addr == peer_.sin_addr.s_addr && from.sin_port == udpRtcpDest_.sin_port;
}

void RtspSession::poll(uint32_t nowMs, uint64_t nowUs, FrameHub &hub, const RtspShared &shared) {
  if (!active()) return;
  receive(nowMs);
  if (!active()) return;
  processRequests(nowMs, nowUs, shared);
  if (!active()) return;
  if (playing() && !closeAfterFlush_) {
    stream(nowMs, nowUs, hub, shared);
    if (!active()) return;
  }
  switch (out_.flush(sock_, nowMs, cfg::kSendStallTimeoutMs)) {
    case net::FlushResult::kFailed:
      close("send failed or stalled");
      return;
    case net::FlushResult::kDone:
      if (closeAfterFlush_) {
        close("teardown");
        return;
      }
      break;
    case net::FlushResult::kPending:
      break;
  }
  // A TCP session that is playing is supervised by the send stall detection;
  // every other state relies on client traffic (requests, RTCP receiver reports).
  const bool supervisedBySend = playing() && transport_.tcp;
  if (!supervisedBySend && nowMs - lastActivityMs_ > cfg::kClientIdleTimeoutMs) close("idle timeout");
}

void RtspSession::receive(uint32_t nowMs) {
  for (;;) {
    char scratch[256];
    char *dst = skip_ != 0 ? scratch : rx_ + rxLen_;
    size_t room = skip_ != 0 ? (skip_ < sizeof(scratch) ? skip_ : sizeof(scratch)) : kRxCapacity - rxLen_;
    if (room == 0) return;  // buffer full: processRequests() decides (kTooLarge)
    const net::IoResult r = net::recvSome(sock_, dst, room);
    if (r.status == net::IoStatus::kWouldBlock) return;
    if (r.status != net::IoStatus::kOk) {
      close(r.status == net::IoStatus::kClosed ? "peer closed" : "receive error");
      return;
    }
    lastActivityMs_ = nowMs;
    if (skip_ != 0) {
      skip_ -= r.bytes;
    } else {
      rxLen_ += r.bytes;
    }
  }
}

void RtspSession::processRequests(uint32_t nowMs, uint64_t nowUs, const RtspShared &shared) {
  while (rxLen_ != 0 && !closeAfterFlush_) {
    if (out_.room() < kMaxResponse) return;  // wait until the response fits
    rtsp::Message m;
    const rtsp::ParseStatus st = rtsp::parseMessage(rx_, rxLen_, kRxCapacity, m);
    if (st == rtsp::ParseStatus::kIncomplete) return;
    if (st == rtsp::ParseStatus::kInvalid || st == rtsp::ParseStatus::kTooLarge) {
      // The byte stream cannot be resynchronized: answer and hang up.
      respondRaw(st == rtsp::ParseStatus::kInvalid ? 400 : 413,
                 st == rtsp::ParseStatus::kInvalid ? "Bad Request" : "Request Entity Too Large");
      closeAfterFlush_ = true;
      rxLen_ = 0;
      return;
    }
    if (st == rtsp::ParseStatus::kRequest) {
      handle(m, nowUs, shared);
    } else if (st == rtsp::ParseStatus::kInterleaved) {
      skip_ = m.skipAfter;  // client RTCP over TCP: proof of life, content ignored
      lastActivityMs_ = nowMs;
    }
    memmove(rx_, rx_ + m.consumed, rxLen_ - m.consumed);
    rxLen_ -= m.consumed;
  }
}

void RtspSession::respond(const rtsp::Message &m, int code, const char *reason, const char *headers,
                          const char *body, size_t bodyLen) {
  uint8_t *p = out_.reserve(kMaxResponse);
  if (p == nullptr) {  // cannot happen: processRequests() checks room first
    close("response buffer full");
    return;
  }
  char *text = reinterpret_cast<char *>(p);
  int n = snprintf(text, kMaxResponse, "RTSP/1.0 %d %s\r\nCSeq: %.*s\r\nServer: %s\r\n%s", code, reason,
                   svLen(m.cseq, 16), m.cseq.data(), cfg::kServerName, headers);
  if (body != nullptr && n > 0 && size_t(n) < kMaxResponse) {
    n += snprintf(text + n, kMaxResponse - size_t(n), "Content-Length: %u\r\n", unsigned(bodyLen));
  }
  if (n <= 0 || size_t(n) + 2 + bodyLen > kMaxResponse) {
    log_e("RTSP response too large");
    close("internal error");
    return;
  }
  memcpy(p + n, "\r\n", 2);
  n += 2;
  if (body != nullptr) {
    memcpy(p + n, body, bodyLen);
    n += int(bodyLen);
  }
  out_.commit(size_t(n));
}

void RtspSession::respondRaw(int code, const char *reason) {
  char buf[128];
  const int n = snprintf(buf, sizeof(buf), "RTSP/1.0 %d %s\r\nServer: %s\r\n\r\n", code, reason, cfg::kServerName);
  if (n > 0 && size_t(n) < sizeof(buf)) out_.append(buf, size_t(n));
}

void RtspSession::localIp(char *out, size_t size) const {
  sockaddr_in local = {};
  socklen_t len = sizeof(local);
  if (getsockname(sock_.fd(), reinterpret_cast<sockaddr *>(&local), &len) != 0) local.sin_addr.s_addr = 0;
  net::formatIp(local.sin_addr.s_addr, out, size);
}

bool RtspSession::checkSession(const rtsp::Message &m) {
  if (sessionId_[0] == '\0' || m.session != sessionId_) {
    respond(m, 454, "Session Not Found");
    return false;
  }
  return true;
}

void RtspSession::handle(const rtsp::Message &m, uint64_t nowUs, const RtspShared &shared) {
  if (m.cseq.empty()) {
    respondRaw(400, "Bad Request");
    return;
  }
  log_d("RTSP %.*s %.*s", svLen(m.method, 16), m.method.data(), svLen(m.uri, kMaxUri), m.uri.data());
  const std::string_view method = m.method;
  if (method == "OPTIONS") {
    respond(m, 200, "OK", "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n");
  } else if (method == "DESCRIBE") {
    handleDescribe(m);
  } else if (method == "SETUP") {
    handleSetup(m, shared);
  } else if (method == "PLAY") {
    handlePlay(m, nowUs);
  } else if (method == "PAUSE") {
    if (!checkSession(m)) return;
    if (state_ == State::kPlaying) {
      state_ = State::kReady;
      dropFrame();
    }
    respond(m, 200, "OK");
  } else if (method == "TEARDOWN") {
    if (!checkSession(m)) return;
    respond(m, 200, "OK");
    state_ = State::kInit;
    closeAfterFlush_ = true;
  } else if (method == "GET_PARAMETER") {
    if (!m.session.empty() && !checkSession(m)) return;
    respond(m, 200, "OK");  // keep-alive
  } else {
    respond(m, 501, "Not Implemented");
  }
}


void RtspSession::handleDescribe(const rtsp::Message &m) {
  if (rtsp::classifyUri(m.uri, cfg::kRtspPath, kTrack) != rtsp::UriTarget::kStream) {
    respond(m, 404, "Not Found");
    return;
  }
  char ip[16];
  localIp(ip, sizeof(ip));
  // Minimal, exact SDP (RFC 4566 / RFC 2435): nothing a player cannot use.
  char sdp[kMaxSdp];
  const int sdpLen = snprintf(sdp, sizeof(sdp),
                              "v=0\r\n"
                              "o=- %u 1 IN IP4 %s\r\n"
                              "s=ESP32-CAM\r\n"
                              "c=IN IP4 0.0.0.0\r\n"
                              "t=0 0\r\n"
                              "a=control:*\r\n"
                              "a=range:npt=0-\r\n"
                              "m=video 0 RTP/AVP %u\r\n"
                              "a=rtpmap:%u JPEG/%u\r\n"
                              "a=framerate:%u\r\n"
                              "a=control:%s\r\n",
                              unsigned(esp_random() >> 1), ip, unsigned(rtpjpeg::kPayloadTypeJpeg),
                              unsigned(rtpjpeg::kPayloadTypeJpeg), unsigned(rtpjpeg::kClockRateHz),
                              unsigned(cfg::kTargetFps), kTrack);
  if (sdpLen <= 0 || size_t(sdpLen) >= sizeof(sdp)) {
    respond(m, 500, "Internal Server Error");
    return;
  }
  char headers[160];
  snprintf(headers, sizeof(headers), "Content-Type: application/sdp\r\nContent-Base: rtsp://%s:%u/%s/\r\n", ip,
           unsigned(cfg::kRtspPort), cfg::kRtspPath);
  respond(m, 200, "OK", headers, sdp, size_t(sdpLen));
}

void RtspSession::handleSetup(const rtsp::Message &m, const RtspShared &shared) {
  if (rtsp::classifyUri(m.uri, cfg::kRtspPath, kTrack) == rtsp::UriTarget::kNone) {
    respond(m, 404, "Not Found");
    return;
  }
  if (sessionId_[0] != '\0' && m.session != sessionId_) {
    respond(m, 454, "Session Not Found");
    return;
  }
  if (state_ == State::kPlaying) {
    respond(m, 455, "Method Not Valid in This State");
    return;
  }
  rtsp::Transport t;
  const bool udpAvailable = shared.udpRtp != nullptr && shared.udpRtp->valid();
  if (!rtsp::parseTransport(m.transport, t) || (!t.tcp && !udpAvailable)) {
    respond(m, 461, "Unsupported Transport");
    return;
  }
  transport_ = t;
  if (!t.tcp) {
    udpRtpDest_ = peer_;
    udpRtpDest_.sin_port = htons(t.clientRtpPort);
    udpRtcpDest_ = peer_;
    udpRtcpDest_.sin_port = htons(t.clientRtcpPort);
  }
  if (sessionId_[0] == '\0') snprintf(sessionId_, sizeof(sessionId_), "%08X", unsigned(esp_random()));
  // Random SSRC, sequence and timestamp origin (RFC 3550 5.1).
  packetizer_ = rtpjpeg::RtpJpegPacketizer(esp_random(), uint16_t(esp_random()), cfg::kRtpMaxPayload);
  rtpBase_ = esp_random();
  packetCount_ = 0;
  octetCount_ = 0;
  state_ = State::kReady;

  char headers[220];
  if (t.tcp) {
    snprintf(headers, sizeof(headers),
             "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;ssrc=%08X\r\nSession: %s;timeout=%u\r\n",
             t.rtpChannel, t.rtcpChannel, unsigned(packetizer_.ssrc()), sessionId_,
             unsigned(cfg::kRtspSessionTimeoutS));
  } else {
    snprintf(headers, sizeof(headers),
             "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u;ssrc=%08X\r\n"
             "Session: %s;timeout=%u\r\n",
             t.clientRtpPort, t.clientRtcpPort, shared.udpServerPort, unsigned(shared.udpServerPort + 1),
             unsigned(packetizer_.ssrc()), sessionId_, unsigned(cfg::kRtspSessionTimeoutS));
  }
  respond(m, 200, "OK", headers);
}

void RtspSession::handlePlay(const rtsp::Message &m, uint64_t nowUs) {
  if (!checkSession(m)) return;
  if (state_ == State::kInit) {
    respond(m, 455, "Method Not Valid in This State");
    return;
  }
  char ip[16];
  localIp(ip, sizeof(ip));
  char headers[240];
  snprintf(headers, sizeof(headers),
           "Session: %s;timeout=%u\r\nRange: npt=0.000-\r\nRTP-Info: url=rtsp://%s:%u/%s/%s;seq=%u;rtptime=%u\r\n",
           sessionId_, unsigned(cfg::kRtspSessionTimeoutS), ip, unsigned(cfg::kRtspPort), cfg::kRtspPath, kTrack,
           unsigned(packetizer_.nextSequence()), unsigned(rtpTimeAt(nowUs)));
  respond(m, 200, "OK", headers);
  if (state_ != State::kPlaying) {
    state_ = State::kPlaying;
    lastSequence_ = 0;  // start with the newest frame
    lastSenderReportMs_ = 0;
  }
}


void RtspSession::dropFrame() {
  packetizer_.abort();  // first: the packetizer points into the slot
  frame_.reset();
  udpPending_ = 0;
}

bool RtspSession::nextFrame(FrameHub &hub) {
  dropFrame();
  FrameRef f = hub.latestAfter(lastSequence_);
  if (!f) return false;
  lastSequence_ = f->sequence;
  if (!f->rtpValid) return false;  // RFC 2435 cannot carry it: skip
  frame_ = static_cast<FrameRef &&>(f);
  packetizer_.begin(frame_->rtp, rtpTimeAt(frame_->captureUs));
  return true;
}

void RtspSession::stream(uint32_t nowMs, uint64_t nowUs, FrameHub &hub, const RtspShared &shared) {
  if (nowMs - lastSenderReportMs_ >= kSenderReportIntervalMs && packetCount_ != 0) {
    sendSenderReport(nowMs, nowUs, shared);
  }

  if (transport_.tcp) {
    // Interleaved: "$" + channel + length, then the RTP packet, straight into
    // the outbox. Only whole packets are queued, so a partial socket write
    // never breaks the framing.
    for (;;) {
      // Keep room for a response while a request is waiting (e.g. TEARDOWN).
      const size_t reserveForReply = rxLen_ != 0 ? kMaxResponse : 0;
      if (out_.room() < 4 + packetizer_.maxPacketSize() + reserveForReply) return;
      if (!packetizer_.hasNext() && !nextFrame(hub)) return;
      uint8_t *p = out_.reserve(4 + packetizer_.maxPacketSize());
      if (p == nullptr) return;  // socket busy: resume on next poll
      const size_t n = packetizer_.next(p + 4, packetizer_.maxPacketSize());
      p[0] = '$';
      p[1] = transport_.rtpChannel;
      p[2] = uint8_t(n >> 8);
      p[3] = uint8_t(n);
      out_.commit(4 + n);
      ++packetCount_;
      octetCount_ += uint32_t(n - rtpjpeg::kRtpHeaderSize);
    }
  }

  // UDP: a datagram refused by lwIP (out of buffers) is retried as-is; if the
  // stack stays congested the rest of the frame is dropped.
  for (size_t sent = 0; sent < kMaxRtspPacketsPerPoll; ++sent) {
    if (udpPending_ == 0) {
      if (!packetizer_.hasNext() && !nextFrame(hub)) return;
      udpPending_ = packetizer_.next(udpPacket_, sizeof(udpPacket_));
      if (udpPending_ == 0) return;
    }
    const net::IoResult r = net::sendTo(*shared.udpRtp, udpPacket_, udpPending_, udpRtpDest_);
    if (r.status == net::IoStatus::kWouldBlock) {
      if (udpStallSinceMs_ == 0) udpStallSinceMs_ = nowMs | 1;
      if (nowMs - udpStallSinceMs_ > cfg::kSendStallTimeoutMs / 3) {
        dropFrame();  // the receiver discards the incomplete frame
        udpStallSinceMs_ = 0;
      }
      return;
    }
    udpStallSinceMs_ = 0;
    if (r.status == net::IoStatus::kOk) {
      ++packetCount_;
      octetCount_ += uint32_t(udpPending_ - rtpjpeg::kRtpHeaderSize);
    }
    udpPending_ = 0;  // a hard error drops the packet, like the network would
  }
}

void RtspSession::sendSenderReport(uint32_t nowMs, uint64_t nowUs, const RtspShared &shared) {
  rtpjpeg::SenderReport sr;
  sr.ssrc = packetizer_.ssrc();
  sr.ntpMicros = nowUs;
  sr.rtpTimestamp = rtpTimeAt(nowUs);
  sr.packetCount = packetCount_;
  sr.octetCount = octetCount_;
  char cname[40];
  char ip[16];
  localIp(ip, sizeof(ip));
  snprintf(cname, sizeof(cname), "%s@%s", cfg::kHostname, ip);

  if (transport_.tcp) {
    uint8_t *p = out_.reserve(4 + rtpjpeg::kMaxRtcpSize);
    if (p == nullptr) return;  // try again on the next poll
    const size_t n = rtpjpeg::buildSenderReport(p + 4, rtpjpeg::kMaxRtcpSize, sr, cname);
    p[0] = '$';
    p[1] = transport_.rtcpChannel;
    p[2] = uint8_t(n >> 8);
    p[3] = uint8_t(n);
    out_.commit(4 + n);
  } else if (shared.udpRtcp != nullptr && shared.udpRtcp->valid()) {
    uint8_t buf[rtpjpeg::kMaxRtcpSize];
    const size_t n = rtpjpeg::buildSenderReport(buf, sizeof(buf), sr, cname);
    net::sendTo(*shared.udpRtcp, buf, n, udpRtcpDest_);
  }
  lastSenderReportMs_ = nowMs;
}

