#include <WiFi.h>

#include "HttpMjpegServer.h"

#include <esp32-hal-log.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Camera.h"
#include "Onvif.h"
#include "Ptz.h"

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

void HttpMjpegServer::handleRequest(Client &c, FrameHub &hub, size_t rtspSessions) {
  char response[640];
  int n = 0;
  if (isGet(c.rx, "/status")) {
    const FrameHub::Stats &t = hub.peekTotals();
    char body[448];
    const int b = snprintf(body, sizeof(body),
                           "{\"camera\":{\"running\":%s,\"pid\":\"0x%04x\",\"initError\":%d},"
                           "\"frames\":{\"captured\":%lu,\"lastSize\":%u,\"captureFailed\":%lu,"
                           "\"rtpRejected\":%lu,\"noFreeSlot\":%lu,\"lastJpegError\":\"%s\"},"
                           "\"mem\":{\"heap\":%u,\"psram\":%u},"
                           "\"net\":{\"rssi\":%d,\"channel\":%d,\"txPower\":%d,\"rtspSessions\":%u},"
                           "\"sys\":{\"uptimeS\":%lu,\"ip\":\"%s\",\"mac\":\"%s\"},"
                           "\"ptz\":{\"running\":%s,\"pan\":%d,\"tilt\":%d}}\n",
                           camera::running() ? "true" : "false", unsigned(camera::sensorPid()),
                           camera::lastInitError(), static_cast<unsigned long>(t.captured),
                           unsigned(t.lastSize), static_cast<unsigned long>(t.captureFailed),
                           static_cast<unsigned long>(t.rtpRejected),
                           static_cast<unsigned long>(t.noFreeSlot),
                           rtpjpeg::toString(hub.lastJpegError()),
                           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), WiFi.RSSI(), int(WiFi.channel()),
                           int(WiFi.getTxPower()), unsigned(rtspSessions),
                           static_cast<unsigned long>(millis() / 1000), WiFi.localIP().toString().c_str(),
                           WiFi.macAddress().c_str(), ptz::running() ? "true" : "false",
                           ptz::pan(), ptz::tilt());
    n = snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                 "Content-Length: %d\r\n\r\n%s",
                 b, body);
    c.mode = Mode::kClosing;
  } else if (isGet(c.rx, "/ptz/home")) {
    ptz::home();
    const int b = snprintf(response + 200, sizeof(response) - 200,
                           "{\"pan\":%d,\"tilt\":%d}\n", ptz::pan(), ptz::tilt());
    n = snprintf(response, 200,
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                 "Content-Length: %d\r\n\r\n",
                 b);
    memcpy(response + n, response + 200, size_t(b));
    n += b;
    c.mode = Mode::kClosing;
  } else if (isGet(c.rx, "/ptz")) {
    const char *q = strstr(c.rx, "/ptz?");
    if (q != nullptr) {
      const char *pp = strstr(q, "pan=");
      const char *tt = strstr(q, "tilt=");
      if (pp != nullptr) ptz::setPan(atoi(pp + 4));
      if (tt != nullptr) ptz::setTilt(atoi(tt + 5));
    }
    const int b = snprintf(response + 200, sizeof(response) - 200,
                           "{\"pan\":%d,\"tilt\":%d}\n", ptz::pan(), ptz::tilt());
    n = snprintf(response, 200,
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                 "Content-Length: %d\r\n\r\n",
                 b);
    memcpy(response + n, response + 200, size_t(b));
    n += b;
    c.mode = Mode::kClosing;
  } else if (startsWith(c.rx, "POST /onvif/")) {
    if (c.onvifBody == nullptr) {
      c.onvifBody = (uint8_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
      if (c.onvifBody == nullptr) {
        static const char e[] = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        net::sendSome(c.sock, e, sizeof(e) - 1);
        close(c);
        return;
      }
    }
    const char *cl = strstr(c.rx, "Content-Length:");
    c.onvifLen = cl != nullptr ? uint16_t(atoi(cl + 15)) : 0;
    const char *he = strstr(c.rx, "\r\n\r\n");
    const size_t hdrEnd = he != nullptr ? size_t(he - c.rx) + 4 : c.rxLen;
    size_t already = c.rxLen > hdrEnd ? c.rxLen - hdrEnd : 0;
    if (already > 3072) already = 3072;
    memcpy(c.onvifBody, c.rx + hdrEnd, already);
    c.onvifGot = uint16_t(already);
    c.rxLen = 0;
    c.mode = Mode::kOnvifRx;
    return;
  } else if (isGet(c.rx, "/mjpeg")) {
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
                           "<p><a href=\"/mjpeg\">MJPEG</a> | <a href=\"/jpg\">Snapshot</a> | "
                           "<a href=\"/status\">Status</a></p>"
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
    const bool onvifRx = c.mode == Mode::kOnvifRx;
    char *dst = collecting ? c.rx + c.rxLen
                           : onvifRx ? (char *)(c.onvifBody + c.onvifGot) : scratch;
    size_t room = collecting ? sizeof(c.rx) - 1 - c.rxLen
                             : onvifRx ? 3072 - c.onvifGot : sizeof(scratch);
    if (room == 0) {
      close(c);
      return;
    }
    const net::IoResult r = net::recvSome(c.sock, dst, room);
    if (r.status == net::IoStatus::kWouldBlock) break;
    if (r.status != net::IoStatus::kOk) {
      close(c);
      return;
    }
    if (collecting) c.rxLen += r.bytes;
    if (onvifRx) c.onvifGot += uint16_t(r.bytes);
  }

  if (c.mode == Mode::kOnvifRx && c.onvifLen > 0 && c.onvifGot >= c.onvifLen) {
    const char *action = strstr(c.rx, "SOAPAction:");
    char actBuf[96] = "";
    if (action != nullptr) {
      const char *q1 = strchr(action + 10, '"');
      const char *q2 = q1 != nullptr ? strchr(q1 + 1, '"') : nullptr;
      if (q1 != nullptr && q2 != nullptr && q2 - q1 - 1 < int(sizeof(actBuf))) {
        memcpy(actBuf, q1 + 1, size_t(q2 - q1 - 1));
      }
    }
    // Render the SOAP body into a PSRAM scratch, then send headers+body.
    static uint8_t *soap = nullptr;
    if (soap == nullptr) soap = (uint8_t *)heap_caps_malloc(6144, MALLOC_CAP_SPIRAM);
    if (soap == nullptr) { close(c); return; }
    const size_t rn = onvif::handle(actBuf[0] ? actBuf : nullptr,
                                    c.onvifBody, c.onvifLen, (char *)soap, 3800);
    if (rn == 0) {
      static const char err[] =
          "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
      c.out.append(err, sizeof(err) - 1);
      c.mode = Mode::kClosing;
      return;
    }
    char hdr[192];
    const int hn = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml\r\n"
                            "Connection: close\r\nContent-Length: %u\r\n\r\n",
                            unsigned(rn));
    c.out.append(hdr, size_t(hn));
    c.out.append(soap, rn);
    c.mode = Mode::kClosing;
    return;
  }

  if (c.mode == Mode::kRequest) {
    c.rx[c.rxLen] = '\0';
    if (strstr(c.rx, "\r\n\r\n") != nullptr) {
      handleRequest(c, hub, rtspSessions);
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
