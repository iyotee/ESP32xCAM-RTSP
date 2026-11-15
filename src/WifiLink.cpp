#include "WifiLink.h"

#include <WiFi.h>

#include "config.h"

namespace wifilink {
namespace {

constexpr uint32_t kForceReconnectAfterMs = 30000;

volatile bool gConnected = false;
uint32_t gDownSinceMs = 0;

void onEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      gConnected = true;
      log_i("WiFi connected, IP %s, RSSI %d dBm", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str(),
            WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (gConnected) log_w("WiFi lost (reason %u)", unsigned(info.wifi_sta_disconnected.reason));
      gConnected = false;
      break;
    default:
      break;
  }
}

}  // namespace

void begin() {
  WiFi.persistent(false);  // do not rewrite credentials to flash on every boot
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg::kHostname);
  WiFi.onEvent(onEvent);
  WiFi.setAutoReconnect(true);
#if defined(WIFI_CHANNEL) && defined(WIFI_BSSID)
  static const uint8_t kBssid[6] = WIFI_BSSID;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL, kBssid);
#else
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif
  WiFi.setSleep(false);
  log_i("WiFi connecting to \"%s\"", WIFI_SSID);
}

bool connected() { return gConnected; }

bool waitConnected(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (!gConnected && millis() - start < timeoutMs) delay(50);
  return gConnected;
}

void poll(uint32_t nowMs) {
  if (gConnected) {
    gDownSinceMs = 0;
    return;
  }
  if (gDownSinceMs == 0) {
    gDownSinceMs = nowMs | 1;
    return;
  }
  if (nowMs - gDownSinceMs > kForceReconnectAfterMs) {
    log_w("WiFi still down after %u s: restarting association", unsigned(kForceReconnectAfterMs / 1000));
    WiFi.disconnect(false, false);
    WiFi.reconnect();
    gDownSinceMs = nowMs | 1;
  }
}

}  // namespace wifilink
