// ESP32xCAM-RTSP - RTSP (RFC 2326 + RFC 2435) and HTTP MJPEG streaming
// for ESP32 camera boards. https://github.com/iyotee/ESP32xCAM-RTSP
//
// Architecture: a single cooperative loop. The FrameHub captures one JPEG per
// frame period (only while somebody watches) and every RTSP / HTTP client
// sends it with non-blocking sockets. Nothing in loop() waits on a peer.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>

#include "Camera.h"
#include "FrameHub.h"
#include "HttpMjpegServer.h"
#include "OtaServer.h"
#include "RtspServer.h"
#include "WifiLink.h"
#include "config.h"

namespace {

constexpr uint32_t kWifiBootTimeoutMs = 30000;
constexpr uint32_t kStatsIntervalMs = 10000;
constexpr uint32_t kCameraRetryMs = 5000;

FrameHub gHub;
RtspServer gRtsp;
HttpMjpegServer gHttp;
bool gStreaming = false;
uint32_t gLastStatsMs = 0;
uint32_t gLastCameraTryMs = 0;
bool gImageConfirmed = false;

void stopStreaming() {
  if (!gStreaming) return;
  gStreaming = false;
  gRtsp.end();
  gHttp.end();
  gHub.releaseMemory();
  camera::end();
  log_i("streaming stopped for firmware update (free heap %u)", unsigned(ESP.getFreeHeap()));
}

OtaServer gOta(stopStreaming);

void logStats(uint32_t nowMs) {
  // [[maybe_unused]]: log_i() compiles to nothing below CORE_DEBUG_LEVEL 3.
  [[maybe_unused]] const FrameHub::Stats s = gHub.takeStats();
  [[maybe_unused]] const float seconds = float(nowMs - gLastStatsMs) / 1000.0f;
  log_i("%.1f fps, last frame %u B, %u RTSP session(s), rejected %u, capture errors %u, heap %u, psram %u, "
        "RSSI %d",
        double(s.captured / seconds), unsigned(s.lastSize), unsigned(gRtsp.sessionCount()), unsigned(s.rtpRejected),
        unsigned(s.captureFailed), unsigned(ESP.getFreeHeap()), unsigned(ESP.getFreePsram()), WiFi.RSSI());
  gLastStatsMs = nowMs;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  log_i("%s starting, reset reason %d", cfg::kServerName, int(esp_reset_reason()));

  wifilink::begin();
  if (!wifilink::waitConnected(kWifiBootTimeoutMs)) {
    // A new image that cannot join the network must not stay installed.
    ota::rollbackIfPending("no WiFi");
    log_w("WiFi not connected yet, continuing (the driver keeps retrying)");
  }

  // The OTA server starts before the camera so that a camera fault can still
  // be fixed remotely.
  gOta.begin();

  gStreaming = gRtsp.begin() && gHttp.begin();
  if (!gStreaming) log_e("cannot open listening sockets");

  gLastCameraTryMs = millis();
  if (!camera::begin()) log_e("camera init failed, retrying every %u s", unsigned(kCameraRetryMs / 1000));

  if (wifilink::connected() && camera::running()) {
    ota::confirmImageIfPending();
    gImageConfirmed = true;
  }
  gLastStatsMs = millis();
  log_i("ready: rtsp://%s:%u/%s  http://%s/mjpeg", WiFi.localIP().toString().c_str(), unsigned(cfg::kRtspPort),
        cfg::kRtspPath, WiFi.localIP().toString().c_str());
}

void loop() {
  const uint32_t nowMs = millis();
  const uint64_t nowUs = uint64_t(esp_timer_get_time());

  wifilink::poll(nowMs);
  gOta.poll();
  if (gOta.updating()) return;  // the upload handler owns the CPU

  if (gStreaming) {
    if (!camera::running() && nowMs - gLastCameraTryMs > kCameraRetryMs) {
      gLastCameraTryMs = nowMs;
      camera::begin();
    }
    if (!gImageConfirmed && wifilink::connected() && camera::running()) {
      ota::confirmImageIfPending();
      gImageConfirmed = true;
    }
    gHub.poll(nowUs, gRtsp.wantsFrames() || gHttp.wantsFrames());
    gRtsp.poll(nowMs, nowUs, gHub);
    gHttp.poll(nowMs, gHub, gRtsp.sessionCount());
  }

  if (nowMs - gLastStatsMs >= kStatsIntervalMs) logStats(nowMs);
  // Yield to the idle task (task watchdog) and the network stack.
  delay(1);
}
