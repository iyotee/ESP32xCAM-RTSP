// Two hobby servos on LEDC PWM: GPIO14 = PAN, GPIO15 = TILT.
// Angles are 0-180 degrees; ONVIF callers use the -1..1 space and get
// converted at the HTTP/ONVIF layer.
#pragma once

#include <stdint.h>

namespace ptz {

// Attaches the PWM channels and moves both servos to the home position.
bool begin();
bool running();

// Absolute position, degrees 0-180 (clamped).
void setPan(int deg);
void setTilt(int deg);
int pan();
int tilt();
void home();

// Continuous motion in ONVIF velocity space (-1..1 per axis); 0 stops that axis.
void continuous(float panVel, float tiltVel);
// Advances continuous motion; call once per main-loop iteration.
void poll(uint32_t nowMs);

}  // namespace ptz
