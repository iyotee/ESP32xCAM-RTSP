// ONVIF WS-Discovery (UDP 3702): answers Probe messages with a ProbeMatch so
// NVRs/VMS can find the camera on the network automatically.
#pragma once

namespace onvifdiscovery {

bool begin();  // bind UDP 3702 and join the discovery multicast group
void poll();   // drain pending Probe messages (call every loop iteration)

}  // namespace onvifdiscovery
