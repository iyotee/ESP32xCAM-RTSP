#include "OnvifDiscovery.h"

#include <stdio.h>
#include <string.h>

#include <WiFi.h>
#include <lwip/igmp.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "config.h"

namespace onvifdiscovery {
namespace {

constexpr uint16_t kPort = 3702;
constexpr uint32_t kMulticastAddr = 0xEFFFFFFA;  // 239.255.255.250, network order

int gFd = -1;
char gMacSuffix[8];  // last 3 MAC bytes for a stable-looking URN

// "...MessageID>uuid:X</..." -> copy X
bool extractMessageId(const char *msg, char *out, size_t outSize) {
  const char *p = strstr(msg, "MessageID");
  if (p == nullptr) return false;
  p = strchr(p, '>');
  if (p == nullptr) return false;
  ++p;
  const char *e = strchr(p, '<');
  if (e == nullptr || size_t(e - p) >= outSize) return false;
  memcpy(out, p, size_t(e - p));
  out[e - p] = '\0';
  return true;
}

void sendProbeMatch(const sockaddr_in &to, const char *probeMsgId) {
  char ip[16];
  snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
  char urn[48];
  snprintf(urn, sizeof(urn), "urn:uuid:ESP32CAM-%s", gMacSuffix);
  char msgId[64];
  snprintf(msgId, sizeof(msgId), "uuid:esp32cam-%s-%lu", gMacSuffix, (unsigned long)millis());

  char resp[2048];
  const int n = snprintf(resp, sizeof(resp),
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
      "xmlns:a=\"http://www.w3.org/2005/08/addressing\" "
      "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
      "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
      "<s:Header>"
      "<a:MessageID>%s</a:MessageID>"
      "<a:RelatesTo>%s</a:RelatesTo>"
      "<a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:To>"
      "<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action>"
      "</s:Header><s:Body><d:ProbeMatches><d:ProbeMatch>"
      "<d:EndpointReference><a:Address>%s</a:Address></d:EndpointReference>"
      "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
      "<d:Scopes>onvif://www.onvif.org/type/video_encoder "
      "onvif://www.onvif.org/type/PTZ onvif://www.onvif.org/hardware/ESP32CAM "
      "onvif://www.onvif.org/name/ESP32xCAM</d:Scopes>"
      "<d:XAddrs>http://%s/onvif/device_service</d:XAddrs>"
      "<d:MetadataVersion>1</d:MetadataVersion>"
      "</d:ProbeMatch></d:ProbeMatches></s:Body></s:Envelope>",
      msgId, probeMsgId[0] ? probeMsgId : "uuid:unknown", urn, ip);
  if (n <= 0) return;
  sendto(gFd, resp, size_t(n), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
}

}  // namespace

bool begin() {
  gFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (gFd < 0) return false;

  int one = 1;
  setsockopt(gFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  const int flags = fcntl(gFd, F_GETFL, 0);
  fcntl(gFd, F_SETFL, flags | O_NONBLOCK);

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(gFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(gFd);
    gFd = -1;
    return false;
  }

  // Join 239.255.255.250 on the STA interface.
  ip_mreq mreq = {};
  mreq.imr_multiaddr.s_addr = htonl(kMulticastAddr);
  mreq.imr_interface.s_addr = WiFi.localIP();
  setsockopt(gFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

  // Stable suffix from the MAC for the UUID/URN.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(gMacSuffix, sizeof(gMacSuffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);

  log_i("WS-Discovery listening on UDP %u", unsigned(kPort));
  return true;
}

void poll() {
  if (gFd < 0) return;
  for (int i = 0; i < 4; ++i) {  // bounded drain per loop iteration
    char buf[3072];
    sockaddr_in from = {};
    socklen_t flen = sizeof(from);
    const ssize_t n = recvfrom(gFd, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<sockaddr *>(&from), &flen);
    if (n <= 0) return;
    buf[n] = '\0';
    // A Probe has "/Probe" in the WS-Addressing action and no "ProbeMatch".
    if (!strstr(buf, "/Probe") || strstr(buf, "ProbeMatch")) continue;
    char msgId[80] = "";
    extractMessageId(buf, msgId, sizeof(msgId));
    sendProbeMatch(from, msgId);
    log_i("WS-Discovery Probe answered");
  }
}

}  // namespace onvifdiscovery
