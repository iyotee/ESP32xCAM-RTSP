#include "Onvif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <WiFi.h>

#include "Ptz.h"
#include "config.h"

namespace onvif {
namespace {

// ONVIF position space is -1..1 on each axis; servos are 0..180 deg.
float posToOnvif(int deg) { return deg / 90.0f - 1.0f; }
int onvifToDeg(float v) {
  int deg = int((v + 1.0f) * 90.0f + (v >= 0 ? 0.5f : -0.5f));
  return deg < 0 ? 0 : (deg > 180 ? 180 : deg);
}

struct Preset {
  char token[24];
  int pan, tilt;
};
Preset gPresets[8];
int gPresetCount = 0;

void ipString(char *out, size_t n) { snprintf(out, n, "%s", WiFi.localIP().toString().c_str()); }

// Finds `name="value"` inside the SOAP body and returns a pointer to `value`
// (null-terminated copy is NOT made; callers bound the parse themselves).
const char *findAttr(const char *body, const char *name) {
  char pat[32];
  snprintf(pat, sizeof(pat), "%s=\"", name);
  const char *p = strstr(body, pat);
  return p == nullptr ? nullptr : p + strlen(pat);
}

double attrDouble(const char *body, const char *name, double dflt) {
  const char *p = findAttr(body, name);
  if (p == nullptr) return dflt;
  return strtod(p, nullptr);
}

// Element text: <tt:Name>VALUE</tt:Name> (prefix-agnostic via '>')
const char *elemText(const char *body, const char *name) {
  char open[48], close[48];
  snprintf(open, sizeof(open), ">%s<", name);
  const char *p = strstr(body, open);
  if (p == nullptr) return nullptr;
  p += strlen(open);
  snprintf(close, sizeof(close), "</");
  const char *end = strstr(p, close);
  if (end == nullptr || end - p > 40) return nullptr;
  static char buf[48];
  const size_t n = size_t(end - p);
  memcpy(buf, p, n);
  buf[n] = '\0';
  return buf;
}

size_t wrapEnvelope(char *out, size_t outSize, const char *deviceNs, const char *method,
                    const char *responseXml) {
  const char *nsUri = deviceNs[0] == 't' && deviceNs[1] == 'd' && deviceNs[2] == 's'
                          ? "http://www.onvif.org/ver10/device/wsdl"
                          : "http://www.onvif.org/ver20/ptz/wsdl";
  char ip[16];
  ipString(ip, sizeof(ip));
  return snprintf(out, outSize,
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                  "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                  "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
                  "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
                  "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
                  "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">"
                  "<s:Body><%s:%sResponse xmlns:%s=\"%s\">%s</%s:%sResponse></s:Body></s:Envelope>",
                  deviceNs, method, deviceNs, nsUri,
                  responseXml, deviceNs, method);
}

// ---- device service -------------------------------------------------------

size_t getDeviceInformation(char *out, size_t n) {
  char xml[512];
  snprintf(xml, sizeof(xml),
           "<tds:Manufacturer>ESP32xCAM</tds:Manufacturer>"
           "<tds:Model>ESP32-CAM</tds:Model>"
           "<tds:FirmwareVersion>%s</tds:FirmwareVersion>"
           "<tds:SerialNumber>ESP32CAM-001</tds:SerialNumber>"
           "<tds:HardwareId>ESP32CAM</tds:HardwareId>",
           cfg::kServerName);
  return wrapEnvelope(out, n, "tds", "GetDeviceInformation", xml);
}

size_t getServices(char *out, size_t n) {
  char ip[16];
  ipString(ip, sizeof(ip));
  char xml[1024];
  snprintf(xml, sizeof(xml),
           "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
           "<tds:XAddr>http://%s/onvif/device_service</tds:XAddr><tds:Version>10.0</tds:Version></tds:Service>"
           "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl</tds:Namespace>"
           "<tds:XAddr>http://%s/onvif/ptz_service</tds:XAddr><tds:Version>20.0</tds:Version></tds:Service>",
           ip, ip);
  return wrapEnvelope(out, n, "tds", "GetServices", xml);
}

size_t getCapabilities(char *out, size_t n) {
  char ip[16];
  ipString(ip, sizeof(ip));
  char xml[2048];
  snprintf(xml, sizeof(xml),
           "<tds:Capabilities><tds:Device><tt:Network><tt:IPFilter>false</tt:IPFilter>"
           "<tt:ZeroConfiguration>false</tt:ZeroConfiguration><tt:IPVersion6>false</tt:IPVersion6>"
           "<tt:DynDNS>false</tt:DynDNS></tt:Network><tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve>"
           "<tt:RemoteDiscovery>false</tt:RemoteDiscovery></tt:System><tt:IO>false</tt:IO>"
           "<tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2>"
           "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration>"
           "<tt:AccessPolicyConfig>false</tt:AccessPolicyConfig><tt:XAddr>http://%s/onvif/device_service</tt:XAddr>"
           "</tt:Security></tds:Device>"
           "<tds:Media><tt:XAddr>http://%s/onvif/device_service</tt:XAddr><tt:StreamingCapabilities>"
           "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP><tt:RTP_RTSP_UDP>true</tt:RTP_RTSP_UDP>"
           "<tt:RTSP>true</tt:RTSP></tt:StreamingCapabilities></tds:Media>"
           "<tds:PTZ><tt:XAddr>http://%s/onvif/ptz_service</tt:XAddr></tds:PTZ></tds:Capabilities>",
           ip, ip, ip);
  return wrapEnvelope(out, n, "tds", "GetCapabilities", xml);
}

// ---- PTZ service ----------------------------------------------------------

const char *ptzNodeXml() {
  static char xml[1024];
  snprintf(xml, sizeof(xml),
           "<tt:PTZNode token=\"ptz\"><tt:Name>PTZ</tt:Name>"
           "<tt:SupportedPTZSpaces><tt:PTZSpaces><tt:PanTilt x=\"true\" y=\"true\"/>"
           "<tt:Zoom>false</tt:Zoom></tt:PTZSpaces></tt:SupportedPTZSpaces>"
           "<tt:MaximumNumberOfPresets>8</tt:MaximumNumberOfPresets>"
           "<tt:HomeSupported>true</tt:HomeSupported></tt:PTZNode>");
  return xml;
}

size_t getNodes(char *out, size_t n) {
  char xml[1400];
  snprintf(xml, sizeof(xml), "%s", ptzNodeXml());
  return wrapEnvelope(out, n, "trt", "GetNodes", xml);
}

size_t getConfiguration(char *out, size_t n) {
  char xml[2048];
  snprintf(xml, sizeof(xml),
           "<trt:PTZConfiguration token=\"ptz\"><tt:Name>PTZ</tt:Name><tt:UseCount>1</tt:UseCount>"
           "<tt:NodeToken>ptz</tt:NodeToken>"
           "<tt:DefaultPTZSpeed><tt:PanTilt x=\"0.5\" y=\"0.5\"/><tt:Zoom>0</tt:Zoom></tt:DefaultPTZSpeed>"
           "<tt:PanTiltLimits><tt:Range><tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace</tt:URI>"
           "<tt:XRange min=\"-1\" max=\"1\"/><tt:YRange min=\"-1\" max=\"1\"/></tt:Range></tt:PanTiltLimits>"
           "<tt:ZoomLimits><tt:Range><tt:URI>http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace</tt:URI>"
           "<tt:XRange min=\"0\" max=\"0\"/></tt:Range></tt:ZoomLimits>"
           "<tt:Presets token=\"presets\"><tt:Preset token=\"home\"><tt:Name>Home</tt:Name>"
           "<tt:PTZPosition><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/><tt:Zoom>0</tt:Zoom></tt:PTZPosition>"
           "</tt:Preset></tt:Presets></trt:PTZConfiguration>",
           posToOnvif(cfg::kServoHomeDeg), posToOnvif(cfg::kServoHomeDeg));
  return wrapEnvelope(out, n, "trt", "GetConfiguration", xml);
}

size_t getStatus(char *out, size_t n) {
  char xml[1024];
  snprintf(xml, sizeof(xml),
           "<trt:PTZStatus><tt:Position><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/><tt:Zoom>0</tt:Zoom>"
           "</tt:Position><tt:MoveStatus><tt:PanTilt>0</tt:PanTilt><tt:Zoom>0</tt:Zoom></tt:MoveStatus>"
           "<tt:Uptime>0</tt:Uptime></trt:PTZStatus>",
           posToOnvif(ptz::pan()), posToOnvif(ptz::tilt()));
  return wrapEnvelope(out, n, "trt", "GetStatus", xml);
}

size_t getPresets(char *out, size_t n) {
  char xml[2048];
  size_t used = snprintf(xml, sizeof(xml), "<trt:Preset token=\"home\"><tt:Name>Home</tt:Name>"
                         "<tt:PTZPosition><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/><tt:Zoom>0</tt:Zoom>"
                         "</tt:PTZPosition></trt:Preset>",
                         posToOnvif(cfg::kServoHomeDeg), posToOnvif(cfg::kServoHomeDeg));
  for (int i = 0; i < gPresetCount && used + 300 < sizeof(xml); ++i) {
    used += snprintf(xml + used, sizeof(xml) - used,
                     "<trt:Preset token=\"%s\"><tt:Name>%s</tt:Name>"
                     "<tt:PTZPosition><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/><tt:Zoom>0</tt:Zoom>"
                     "</tt:PTZPosition></trt:Preset>",
                     gPresets[i].token, gPresets[i].token, posToOnvif(gPresets[i].pan),
                     posToOnvif(gPresets[i].tilt));
  }
  return wrapEnvelope(out, n, "trt", "GetPresets", xml);
}

// PanTilt x/y attributes appear in AbsoluteMove, ContinuousMove (as Velocity)
// and preset positions; we only act on the request-level move elements.
size_t absoluteMove(char *out, size_t n, const char *body) {
  if (!strstr(body, "AbsoluteMove")) return 0;
  // Only the direct PanTilt child of the request, not nested preset data:
  // find the first PanTilt after ">" following AbsoluteMove.
  const char *p = strstr(body, "AbsoluteMove");
  const char *pt = strstr(p, "PanTilt");
  if (pt == nullptr) return 0;
  const double x = attrDouble(pt, "x", 0.0);
  const double y = attrDouble(pt, "y", 0.0);
  ptz::setPan(onvifToDeg(float(x)));
  ptz::setTilt(onvifToDeg(float(y)));
  return wrapEnvelope(out, n, "trt", "AbsoluteMove", "");
}

size_t continuousMove(char *out, size_t n, const char *body) {
  if (!strstr(body, "ContinuousMove")) return 0;
  const char *p = strstr(body, "Velocity");
  if (p == nullptr) return 0;
  const char *pt = strstr(p, "PanTilt");
  if (pt == nullptr) return 0;
  ptz::continuous(float(attrDouble(pt, "x", 0.0)), float(attrDouble(pt, "y", 0.0)));
  return wrapEnvelope(out, n, "trt", "ContinuousMove", "");
}

size_t stop(char *out, size_t n, const char *body) {
  if (!strstr(body, "<") || strstr(body, "Stop") == nullptr) return 0;
  // "Stop" also matches "Stop" inside other words; require the element form.
  if (strstr(body, ">Stop<") == nullptr && strstr(body, ":Stop") == nullptr) return 0;
  ptz::continuous(0.0f, 0.0f);
  return wrapEnvelope(out, n, "trt", "Stop", "");
}

size_t gotoPreset(char *out, size_t n, const char *body) {
  if (strstr(body, "GotoPreset") == nullptr) return 0;
  const char *tok = elemText(body, "PresetToken");
  if (tok == nullptr) tok = "home";
  if (strcmp(tok, "home") == 0) {
    ptz::home();
  } else {
    for (int i = 0; i < gPresetCount; ++i) {
      if (strcmp(gPresets[i].token, tok) == 0) {
        ptz::setPan(gPresets[i].pan);
        ptz::setTilt(gPresets[i].tilt);
        break;
      }
    }
  }
  return wrapEnvelope(out, n, "trt", "GotoPreset", "");
}

size_t setPreset(char *out, size_t n, const char *body) {
  if (strstr(body, "SetPreset") == nullptr) return 0;
  const char *tok = elemText(body, "PresetToken");
  if (tok == nullptr || *tok == '\0') return 0;
  for (int i = 0; i < gPresetCount; ++i) {
    if (strcmp(gPresets[i].token, tok) == 0) {
      gPresets[i].pan = ptz::pan();
      gPresets[i].tilt = ptz::tilt();
      return wrapEnvelope(out, n, "trt", "SetPreset", "");
    }
  }
  if (gPresetCount < 8 && strlen(tok) < sizeof(gPresets[0].token)) {
    Preset &p = gPresets[gPresetCount++];
    snprintf(p.token, sizeof(p.token), "%s", tok);
    p.pan = ptz::pan();
    p.tilt = ptz::tilt();
  }
  return wrapEnvelope(out, n, "trt", "SetPreset", "");
}

size_t removePreset(char *out, size_t n, const char *body) {
  if (strstr(body, "RemovePreset") == nullptr) return 0;
  const char *tok = elemText(body, "PresetToken");
  if (tok == nullptr) return 0;
  for (int i = 0; i < gPresetCount; ++i) {
    if (strcmp(gPresets[i].token, tok) == 0) {
      memmove(&gPresets[i], &gPresets[i + 1], size_t(gPresetCount - i - 1) * sizeof(Preset));
      --gPresetCount;
      break;
    }
  }
  return wrapEnvelope(out, n, "trt", "RemovePreset", "");
}

}  // namespace

size_t handle(const char *soapAction, const uint8_t *body, size_t bodyLen, char *out,
              size_t outSize) {
  // Copy into a NUL-terminated scratch: strstr helpers need it.
  static char req[3200];
  if (bodyLen >= sizeof(req)) return 0;
  memcpy(req, body, bodyLen);
  req[bodyLen] = '\0';

  // Dispatch on the method name inside the body (prefix-agnostic). Order the
  // checks so longer/specific names win over substrings.
  size_t n = 0;
  if ((n = getDeviceInformation(out, outSize)) && strstr(req, "GetDeviceInformation")) return n;
  n = 0;
  if (strstr(req, "GetServiceCapabilities") || strstr(req, "GetCapabilities")) {
    if (strstr(req, "GetServiceCapabilities")) {
      n = wrapEnvelope(out, outSize, "tds", "GetServiceCapabilities",
                       "<tds:Capabilities><tt:PTZ><tt:XAddr>http://x/ptz</tt:XAddr></tt:PTZ></tds:Capabilities>");
    } else {
      n = getCapabilities(out, outSize);
    }
    return n;
  }
  if (strstr(req, "GetServices")) return getServices(out, outSize);
  if ((n = absoluteMove(out, outSize, req)) != 0) return n;
  if ((n = continuousMove(out, outSize, req)) != 0) return n;
  if ((n = stop(out, outSize, req)) != 0) return n;
  if (strstr(req, "GetNodes")) return getNodes(out, outSize);
  if (strstr(req, "GetNode") || strstr(req, "GetConfiguration")) {
    return strstr(req, "GetConfiguration") ? getConfiguration(out, outSize) : getNodes(out, outSize);
  }
  if (strstr(req, "GetStatus")) return getStatus(out, outSize);
  if (strstr(req, "GetPresets")) return getPresets(out, outSize);
  if ((n = gotoPreset(out, outSize, req)) != 0) return n;
  if ((n = setPreset(out, outSize, req)) != 0) return n;
  if ((n = removePreset(out, outSize, req)) != 0) return n;
  if (strstr(req, "GetEndpointReference") || strstr(req, "GetScopes") ||
      strstr(req, "GetSystemDateAndTime") || strstr(req, "GetWsdlUrl")) {
    // Acknowledge common discovery/metadata calls with an empty response body.
    const char *m = strstr(req, "GetEndpointReference") ? "GetEndpointReference"
                    : strstr(req, "GetScopes")             ? "GetScopes"
                    : strstr(req, "GetSystemDateAndTime")  ? "GetSystemDateAndTime"
                                                        : "GetWsdlUrl";
    const char *ns = strstr(m, "GetWsdlUrl") ? "tds" : "tds";
    return wrapEnvelope(out, outSize, ns, m, "");
  }
  (void)soapAction;
  return 0;
}

}  // namespace onvif
