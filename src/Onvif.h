// Minimal ONVIF device: enough of the device and PTZ services for an NVR/VMS
// to add the camera by IP and drive the servos. No WS-Discovery yet.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace onvif {

// Parses a complete SOAP request and writes the SOAP response envelope into
// `out` (returns its length, 0 when the method/path is unsupported).
// `soapAction` may be nullptr; dispatch falls back to the body element name.
size_t handle(const char *soapAction, const uint8_t *body, size_t bodyLen, char *out,
              size_t outSize);

}  // namespace onvif
