// One RTSP client connection (RFC 2326) streaming RTP/JPEG over TCP
// interleaved or UDP unicast.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "FrameHub.h"
#include "RtpJpegPacketizer.h"
#include "RtspParser.h"
#include "net/Outbox.h"
#include "net/Socket.h"

// Resources shared by all sessions, owned by RtspServer.
struct RtspShared {
  net::Socket *udpRtp = nullptr;   // server_port (RTP)
  net::Socket *udpRtcp = nullptr;  // server_port + 1 (RTCP)
  uint16_t udpServerPort = 0;
};

class RtspSession {
 public:
  bool active() const { return sock_.valid(); }
  bool playing() const { return active() && state_ == State::kPlaying; }

  void open(net::Socket sock, const sockaddr_in &peer, uint32_t nowMs);
  void close(const char *reason);
  // Reads requests, sends RTP and RTCP. Closes itself on error or timeout.
  void poll(uint32_t nowMs, uint64_t nowUs, FrameHub &hub, const RtspShared &shared);
  // RTCP from a UDP client (keeps the session alive).
  bool matchesUdpPeer(const sockaddr_in &from) const;
  void touch(uint32_t nowMs) { lastActivityMs_ = nowMs; }

 private:
  enum class State : uint8_t { kInit, kReady, kPlaying };

  static constexpr size_t kRxCapacity = 1024;
  static constexpr size_t kTxCapacity = 8 * 1024;
  static constexpr size_t kMaxResponse = 1024;
  static constexpr size_t kMaxPacket = rtpjpeg::kRtpHeaderSize + 1400;
  static constexpr uint32_t kSenderReportIntervalMs = 5000;

  void receive(uint32_t nowMs);
  void processRequests(uint32_t nowMs, uint64_t nowUs, const RtspShared &shared);
  void handle(const rtsp::Message &m, uint64_t nowUs, const RtspShared &shared);
  void handleDescribe(const rtsp::Message &m);
  void handleSetup(const rtsp::Message &m, const RtspShared &shared);
  void handlePlay(const rtsp::Message &m, uint64_t nowUs);
  bool checkSession(const rtsp::Message &m);

  // Response helpers: status line + CSeq + Server/Date, extra headers, body.
  void respond(const rtsp::Message &m, int code, const char *reason, const char *headers = "",
               const char *body = nullptr, size_t bodyLen = 0);
  void respondRaw(int code, const char *reason);

  void stream(uint32_t nowMs, uint64_t nowUs, FrameHub &hub, const RtspShared &shared);
  bool nextFrame(FrameHub &hub);
  void dropFrame();
  void sendSenderReport(uint32_t nowMs, uint64_t nowUs, const RtspShared &shared);
  uint32_t rtpTimeAt(uint64_t us) const { return rtpBase_ + uint32_t(us * 9 / 100); }
  void localIp(char *out, size_t size) const;

  net::Socket sock_;
  sockaddr_in peer_ = {};
  char rx_[kRxCapacity];
  size_t rxLen_ = 0;
  size_t skip_ = 0;  // remainder of an incoming interleaved frame to discard
  net::Outbox<kTxCapacity> out_;
  bool closeAfterFlush_ = false;
  uint32_t lastActivityMs_ = 0;

  State state_ = State::kInit;
  char sessionId_[9] = {};
  rtsp::Transport transport_;
  sockaddr_in udpRtpDest_ = {};
  sockaddr_in udpRtcpDest_ = {};

  rtpjpeg::RtpJpegPacketizer packetizer_;
  uint32_t rtpBase_ = 0;
  FrameRef frame_;
  uint32_t lastSequence_ = 0;
  uint8_t udpPacket_[kMaxPacket];
  size_t udpPending_ = 0;
  uint32_t udpStallSinceMs_ = 0;
  uint32_t packetCount_ = 0;
  uint32_t octetCount_ = 0;
  uint32_t lastSenderReportMs_ = 0;
};
