// Minimal non-blocking HTTP/1.1 server: "/" status page, "/mjpeg" multipart
// stream and "/jpg" single snapshot. Frames come from the shared FrameHub and
// are sent zero-copy; a browser never blocks the RTSP clients.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "FrameHub.h"
#include "config.h"
#include "net/Outbox.h"
#include "net/Socket.h"

class HttpMjpegServer {
 public:
  bool begin();
  void end();
  void poll(uint32_t nowMs, FrameHub &hub, size_t rtspSessions);
  bool wantsFrames() const;

 private:
  enum class Mode : uint8_t { kRequest, kStream, kSnapshot, kClosing };

  struct Client {
    net::Socket sock;
    Mode mode = Mode::kRequest;
    char rx[512];
    size_t rxLen = 0;
    net::Outbox<1024> out;
    FrameRef frame;
    uint32_t lastSequence = 0;
    uint32_t sinceMs = 0;
  };

  void acceptClients(uint32_t nowMs);
  void pollClient(Client &c, uint32_t nowMs, FrameHub &hub, size_t rtspSessions);
  void handleRequest(Client &c, FrameHub &hub, size_t rtspSessions);
  void queueFrame(Client &c, FrameHub &hub);
  void close(Client &c);

  net::Socket listener_;
  Client clients_[cfg::kMaxHttpClients];
};
