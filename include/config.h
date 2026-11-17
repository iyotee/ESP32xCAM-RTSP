// Build-time configuration. Credentials live in secrets.h (not versioned).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_camera.h>

#if !__has_include("secrets.h")
#error "include/secrets.h is missing: copy include/secrets.h.example and fill it in."
#endif
#include "secrets.h"

namespace cfg {

// ---- Identity ---------------------------------------------------------------
constexpr const char *kHostname = "esp32xcam";
constexpr const char *kServerName = "ESP32xCAM-RTSP/2.0";

// ---- Camera (OV2640) --------------------------------------------------------
constexpr framesize_t kFrameSize = FRAMESIZE_SVGA;  // 800x600
// 0..63, lower = better quality and bigger frames. At SVGA/q10 frames are
// ~25 KB (~380 KB/s at 15 fps): fine for 1-2 clients on a healthy 2.4 GHz link.
constexpr int kJpegQuality = 10;
constexpr int kXclkHz = 20000000;
constexpr size_t kFrameBufferCount = 2;  // driver double-buffering in PSRAM

// ---- Streaming --------------------------------------------------------------
constexpr uint32_t kTargetFps = 30;
constexpr uint32_t kFramePeriodUs = 1000000UL / kTargetFps;

// ---- RTSP -------------------------------------------------------------------
constexpr uint16_t kRtspPort = 8554;
constexpr const char *kRtspPath = "stream=0";  // rtsp://<ip>:8554/stream=0
// lwIP allows 16 sockets: 3 listeners + 2 UDP + 2 HTTP + 1 OTA client + 4 = 12.
constexpr size_t kMaxRtspSessions = 4;
constexpr uint32_t kRtspSessionTimeoutS = 60;  // advertised in "Session: ...;timeout="
constexpr uint16_t kRtpServerPort = 6970;      // even; RTCP uses kRtpServerPort + 1
// RTP payload budget per packet (fits a 1500-byte MTU with IP/UDP/RTP headers).
constexpr size_t kRtpMaxPayload = 1400;

// ---- HTTP MJPEG -------------------------------------------------------------
constexpr uint16_t kHttpPort = 80;
constexpr size_t kMaxHttpClients = 2;

// ---- PTZ (two hobby servos: PAN on GPIO14, TILT on GPIO15) -------------------
constexpr int kPanServoPin = 14;
constexpr int kTiltServoPin = 15;
constexpr int kServoMinPulseUs = 500;   // pulse width at 0 deg
constexpr int kServoMaxPulseUs = 2400;  // pulse width at 180 deg
constexpr int kServoHomeDeg = 90;
constexpr float kPtzSpeedDegPerSec = 60.0f;  // continuous-move speed

// ---- OTA --------------------------------------------------------------------
constexpr uint16_t kOtaPort = 3232;

// ---- Network robustness -----------------------------------------------------
// A socket write that cannot make progress for this long drops the client.
constexpr uint32_t kSendStallTimeoutMs = 1500;
// Clients that sent nothing (RTSP keep-alive / RTCP) for this long are dropped.
constexpr uint32_t kClientIdleTimeoutMs = kRtspSessionTimeoutS * 1000UL * 2;

}  // namespace cfg
