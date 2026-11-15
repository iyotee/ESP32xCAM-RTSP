#include "HttpMjpegServer.h"

#include <esp32-hal-log.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t kRequestTimeoutMs = 5000;
constexpr uint32_t kNoFrameTimeoutMs = 10000;
constexpr char kBoundary[] = "esp32camframe";
constexpr char kBusy[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";

bool startsWith(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

// "GET /path " or "GET /path?query"
bool isGet(const char *request, const char *path) {
  if (!startsWith(request, "GET ")) return false;
  const size_t n = strlen(path);
  return strncmp(request + 4, path, n) == 0 && (request[4 + n] == ' ' || request[4 + n] == '?');
}

}  // namespace

bool HttpMjpegServer::begin() {
  listener_ = net::listenTcp(cfg::kHttpPort, int(cfg::kMaxHttpClients));
  if (!listener_) return false;
  log_i("HTTP server on port %u (/mjpeg, /jpg)", unsigned(cfg::kHttpPort));
  return true;
}

void HttpMjpegServer::end() {
  for (Client &c : clients_) close(c);
  listener_.reset();
}

bool HttpMjpegServer::wantsFrames() const {
  for (const Client &c : clients_) {
    if (c.sock.valid() && (c.mode == Mode::kStream || c.mode == Mode::kSnapshot)) return true;
  }
  return false;
}

void HttpMjpegServer::close(Client &c) {
  c.out.clear();  // first: it may point into the frame slot
  c.frame.reset();
  c.sock.reset();
  c.mode = Mode::kRequest;
  c.rxLen = 0;
}

void HttpMjpegServer::acceptClients(uint32_t nowMs) {
  for (;;) {
    net::Socket s = net::acceptClient(listener_, nullptr);
    if (!s) return;
    Client *slot = nullptr;
    for (Client &c : clients_) {
      if (!c.sock.valid()) {
        slot = &c;
        break;
      }
    }
    if (slot == nullptr) {
      net::sendSome(s, kBusy, sizeof(kBusy) - 1);
      continue;  // Socket destructor closes it
    }
    slot->sock = static_cast<net::Socket &&>(s);
    slot->mode = Mode::kRequest;
    slot->rxLen = 0;
    slot->lastSequence = 0;
    slot->sinceMs = nowMs;
  }
}

void HttpMjpegServer::handleRequest(Client &c, size_t rtspSessions) {
  char response[640];
  int n = 0;
  if (isGet(c.rx, "/mjpeg")) {
    n = snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=%s\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
                 kBoundary);
    c.mode = Mode::kStream;
  } else if (isGet(c.rx, "/jpg")) {
    c.mode = Mode::kSnapshot;  // headers go out with the frame (Content-Length)
    return;
  } else if (isGet(c.rx, "/")) {
    char body[400];
    const int b = snprintf(body, sizeof(body),
                           "<!DOCTYPE html><html><head><title>%s</title></head><body>"
                           "<h1>%s</h1><p>RTSP: rtsp://&lt;ip&gt;:%u/%s - %u session(s)</p>"
                           "<p><a href=\"/mjpeg\">MJPEG</a> | <a href=\"/jpg\">Snapshot</a></p>"
                           "<img src=\"/mjpeg\"></body></html>",
                           cfg::kHostname, cfg::kServerName, unsigned(cfg::kRtspPort), cfg::kRtspPath,
                           unsigned(rtspSessions));
    n = snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s",
                 b, body);
    c.mode = Mode::kClosing;
  } else {
    const bool get = startsWith(c.rx, "GET ");
    n = snprintf(response, sizeof(response), "HTTP/1.1 %s\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
                 get ? "404 Not Found" : "405 Method Not Allowed");
    c.mode = Mode::kClosing;
  }
  if (n <= 0 || size_t(n) >= sizeof(response) || !c.out.append(response, size_t(n))) {
    log_e("HTTP response does not fit");
    c.out.clear();
    c.mode = Mode::kClosing;
  }
}

void HttpMjpegServer::queueFrame(Client &c, FrameHub &hub) {
  c.frame.reset();
  FrameRef f = hub.latestAfter(c.lastSequence);
  if (!f) return;
  char header[200];
  int n;
  if (c.mode == Mode::kStream) {
    // The CRLF ending the previous part is sent as the start of the next
    // delimiter ("\r\n--boundary"), RFC 2046 5.1.1: the JPEG bytes are queued
    // zero-copy and must be the last item of the outbox.
    n = snprintf(header, sizeof(header),
                 "%s--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp-Us: %llu\r\n\r\n",
                 c.lastSequence != 0 ? "\r\n" : "", kBoundary, unsigned(f->length),
                 static_cast<unsigned long long>(f->captureUs));
  } else {
    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                 unsigned(f->length));
    c.mode = Mode::kClosing;
  }
  if (n <= 0 || size_t(n) >= sizeof(header) || !c.out.append(header, size_t(n))) {
    c.mode = Mode::kClosing;
    return;
  }
  c.lastSequence = f->sequence;
  c.frame = static_cast<FrameRef &&>(f);
  c.out.appendExternal(c.frame->data, c.frame->length);
}

void HttpMjpegServer::pollClient(Client &c, uint32_t nowMs, FrameHub &hub, size_t rtspSessions) {
  // Read the request; afterwards just drain input to notice a closed peer.
  for (;;) {
    char scratch[64];
    const bool collecting = c.mode == Mode::kRequest;
    char *dst = collecting ? c.rx + c.rxLen : scratch;
    const size_t room = collecting ? sizeof(c.rx) - 1 - c.rxLen : sizeof(scratch);
    if (room == 0) {
      close(c);  // request headers too large
      return;
    }
    const net::IoResult r = net::recvSome(c.sock, dst, room);
    if (r.status == net::IoStatus::kWouldBlock) break;
    if (r.status != net::IoStatus::kOk) {
      close(c);
      return;
    }
    if (collecting) c.rxLen += r.bytes;
  }

  if (c.mode == Mode::kRequest) {
    c.rx[c.rxLen] = '\0';
    if (strstr(c.rx, "\r\n\r\n") != nullptr) {
      handleRequest(c, rtspSessions);
    } else if (nowMs - c.sinceMs > kRequestTimeoutMs) {
      close(c);
      return;
    }
  }

  // Next frame only once the previous one is completely sent.
  if ((c.mode == Mode::kStream || c.mode == Mode::kSnapshot) && c.out.empty()) {
    const uint32_t before = c.lastSequence;
    queueFrame(c, hub);
    if (c.lastSequence != before) {
      c.sinceMs = nowMs;
    } else if (nowMs - c.sinceMs > kNoFrameTimeoutMs) {
      log_w("HTTP client dropped: no camera frame for %u s", unsigned(kNoFrameTimeoutMs / 1000));
      close(c);
      return;
    }
  }

  switch (c.out.flush(c.sock, nowMs, cfg::kSendStallTimeoutMs)) {
    case net::FlushResult::kFailed:
      close(c);
      return;
    case net::FlushResult::kDone:
      if (c.mode == Mode::kClosing) close(c);
      break;
    case net::FlushResult::kPending:
      break;
  }
}

void HttpMjpegServer::poll(uint32_t nowMs, FrameHub &hub, size_t rtspSessions) {
  if (!listener_) return;
  acceptClients(nowMs);
  for (Client &c : clients_) {
    if (c.sock.valid()) pollClient(c, nowMs, hub, rtspSessions);
  }
}

