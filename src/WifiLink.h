// Station-mode WiFi: connects with the credentials of secrets.h, keeps the
// radio awake (no modem sleep: it adds 100+ ms of latency per packet) and lets
// the driver reconnect by itself. Status changes are reported via events.
#pragma once

#include <stdint.h>

namespace wifilink {

void begin();
bool connected();
// Blocks up to `timeoutMs` for the first IP address.
bool waitConnected(uint32_t timeoutMs);
// Restarts the association if the link stayed down too long (driver stuck).
void poll(uint32_t nowMs);

}  // namespace wifilink
