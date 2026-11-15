#include "RtspServer.h"

#include <esp32-hal-log.h>

namespace {

constexpr char kBusyResponse[] = "RTSP/1.0 503 Service Unavailable\r\n\r\n";

}  // namespace

bool RtspServer::begin() {
  listener_ = net::listenTcp(cfg::kRtspPort, int(cfg::kMaxRtspSessions));
  if (!listener_) return false;
  udpRtp_ = net::bindUdp(cfg::kRtpServerPort);
  udpRtcp_ = net::bindUdp(uint16_t(cfg::kRtpServerPort + 1));
  if (!udpRtp_ || !udpRtcp_) {
    log_w("UDP transport unavailable: clients must use RTSP over TCP");
    udpRtp_.reset();
    udpRtcp_.reset();
  }
  shared_.udpRtp = &udpRtp_;
  shared_.udpRtcp = &udpRtcp_;
  shared_.udpServerPort = cfg::kRtpServerPort;
  log_i("RTSP server on port %u (UDP %s)", unsigned(cfg::kRtspPort), udpRtp_ ? "enabled" : "disabled");
  return true;
}

void RtspServer::end() {
  for (RtspSession &s : sessions_) s.close("server stopped");
  listener_.reset();
  udpRtp_.reset();
  udpRtcp_.reset();
}

bool RtspServer::wantsFrames() const {
  for (const RtspSession &s : sessions_) {
    if (s.playing()) return true;
  }
  return false;
}

size_t RtspServer::sessionCount() const {
  size_t n = 0;
  for (const RtspSession &s : sessions_) n += s.active() ? 1 : 0;
  return n;
}

void RtspServer::acceptClients(uint32_t nowMs) {
  for (;;) {
    sockaddr_in peer = {};
    net::Socket client = net::acceptClient(listener_, &peer);
    if (!client) return;
    RtspSession *slot = nullptr;
    for (RtspSession &s : sessions_) {
      if (!s.active()) {
        slot = &s;
        break;
      }
    }
    if (slot == nullptr) {
      net::sendSome(client, kBusyResponse, sizeof(kBusyResponse) - 1);
      log_w("RTSP client rejected: %u sessions already active", unsigned(cfg::kMaxRtspSessions));
      continue;  // Socket destructor closes it
    }
    slot->open(static_cast<net::Socket &&>(client), peer, nowMs);
  }
}

void RtspServer::drainUdp(uint32_t nowMs) {
  // Incoming RTCP receiver reports keep UDP sessions alive; RTP socket input is
  // discarded so that its receive queue never fills up.
  uint8_t buf[256];
  for (net::Socket *s : {&udpRtcp_, &udpRtp_}) {
    if (!s->valid()) continue;
    for (int i = 0; i < 8; ++i) {
      sockaddr_in from = {};
      socklen_t len = sizeof(from);
      const ssize_t n = recvfrom(s->fd(), buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&from), &len);
      if (n < 0) break;
      for (RtspSession &session : sessions_) {
        if (session.matchesUdpPeer(from)) session.touch(nowMs);
      }
    }
  }
}

void RtspServer::poll(uint32_t nowMs, uint64_t nowUs, FrameHub &hub) {
  if (!listener_) return;
  acceptClients(nowMs);
  drainUdp(nowMs);
  for (RtspSession &s : sessions_) s.poll(nowMs, nowUs, hub, shared_);
}
