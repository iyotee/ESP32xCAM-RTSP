// RTSP listener + fixed pool of sessions. One UDP socket pair is shared by all
// sessions (lwIP allows only 16 sockets in total).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "FrameHub.h"
#include "RtspSession.h"
#include "config.h"
#include "net/Socket.h"

class RtspServer {
 public:
  bool begin();
  void end();
  void poll(uint32_t nowMs, uint64_t nowUs, FrameHub &hub);
  bool wantsFrames() const;
  size_t sessionCount() const;

 private:
  void acceptClients(uint32_t nowMs);
  void drainUdp(uint32_t nowMs);

  net::Socket listener_;
  net::Socket udpRtp_;
  net::Socket udpRtcp_;
  RtspShared shared_;
  RtspSession sessions_[cfg::kMaxRtspSessions];
};
