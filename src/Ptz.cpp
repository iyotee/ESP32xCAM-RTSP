#include "Ptz.h"

#include <Arduino.h>

#include "config.h"

namespace ptz {
namespace {

constexpr int kPanPin = cfg::kPanServoPin;
constexpr int kTiltPin = cfg::kTiltServoPin;
constexpr int kHomeDeg = cfg::kServoHomeDeg;
constexpr int kLedcResolution = 14;  // 50 Hz -> 16384 ticks per 20 ms period

bool gRunning = false;
int gPanDeg = kHomeDeg;
int gTiltDeg = kHomeDeg;
int gPanChannel = -1;
int gTiltChannel = -1;
float gVelPan = 0.0f;   // ONVIF space -1..1
float gVelTilt = 0.0f;
uint32_t gLastPollMs = 0;

int clampDeg(int deg) { return deg < 0 ? 0 : (deg > 180 ? 180 : deg); }

// Hobby servo pulse: ~500-2400 us maps to 0-180 deg (adjust the config if
// your servos hit the mechanical stops earlier).
uint32_t dutyForDeg(int deg) {
  const float pulseUs = float(cfg::kServoMinPulseUs) +
                        float(cfg::kServoMaxPulseUs - cfg::kServoMinPulseUs) * deg / 180.0f;
  return uint32_t(pulseUs * ((1 << kLedcResolution) - 1) / 20000.0f);
}

void writeServos() {
  if (gPanChannel >= 0) ledcWrite(gPanChannel, dutyForDeg(gPanDeg));
  if (gTiltChannel >= 0) ledcWrite(gTiltChannel, dutyForDeg(gTiltDeg));
}

void attach() {
  gPanChannel = ledcAttach(kPanPin, 50, kLedcResolution);
  gTiltChannel = ledcAttach(kTiltPin, 50, kLedcResolution);
}

}  // namespace

bool begin() {
  if (gRunning) return true;
  attach();
  if (gPanChannel < 0 || gTiltChannel < 0) return false;
  gVelPan = gVelTilt = 0.0f;
  gLastPollMs = millis();
  home();
  delay(300);  // let the servos reach home before streaming starts
  gRunning = true;
  return true;
}

bool running() { return gRunning; }

void setPan(int deg) {
  gPanDeg = clampDeg(deg);
  gVelPan = 0.0f;
  writeServos();
}

void setTilt(int deg) {
  gTiltDeg = clampDeg(deg);
  gVelTilt = 0.0f;
  writeServos();
}

int pan() { return gPanDeg; }
int tilt() { return gTiltDeg; }

void home() {
  gPanDeg = gTiltDeg = kHomeDeg;
  writeServos();
}

void continuous(float panVel, float tiltVel) {
  gVelPan = panVel;
  gVelTilt = tiltVel;
}

void poll(uint32_t nowMs) {
  if (!gRunning) return;
  const uint32_t dt = nowMs - gLastPollMs;
  gLastPollMs = nowMs;
  if (gVelPan == 0.0f && gVelTilt == 0.0f) return;
  const float step = cfg::kPtzSpeedDegPerSec * float(dt) / 1000.0f;
  gPanDeg = clampDeg(int(gPanDeg + gVelPan * step + (gVelPan > 0 ? 0.5f : -0.5f)));
  gTiltDeg = clampDeg(int(gTiltDeg + gVelTilt * step + (gVelTilt > 0 ? 0.5f : -0.5f)));
  // Arriving at a stop clamps the axis velocity too.
  if ((gVelPan > 0 && gPanDeg == 180) || (gVelPan < 0 && gPanDeg == 0)) gVelPan = 0;
  if ((gVelTilt > 0 && gTiltDeg == 180) || (gVelTilt < 0 && gTiltDeg == 0)) gVelTilt = 0;
  writeServos();
}

}  // namespace ptz
