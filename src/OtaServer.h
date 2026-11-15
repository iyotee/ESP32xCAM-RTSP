// Firmware update over HTTP (port 3232), protected by HTTP Basic auth.
//
//   curl -u admin:<password> -F firmware=@firmware.bin http://<ip>:3232/upload
//
// Same endpoint as the previous firmware, so tooling keeps working.
#pragma once

#include <WebServer.h>

#include <functional>

class OtaServer {
 public:
  // `stopStreaming` is called once, right before the flash is written, to give
  // the update all the memory and CPU it needs.
  explicit OtaServer(std::function<void()> stopStreaming);
  void begin();
  void poll();
  bool updating() const { return updating_; }

 private:
  bool authorized();
  void handleRoot();
  void handleUploadChunk();
  void handleUploadDone();

  WebServer server_;
  std::function<void()> stopStreaming_;
  bool updating_ = false;
  bool rejected_ = false;
  size_t written_ = 0;
};

// Rollback protection: a freshly flashed image is confirmed only once it has
// proven it works (WiFi up and camera running). Otherwise the bootloader
// returns to the previous image on the next reset (if it supports rollback).
namespace ota {
void confirmImageIfPending();
void rollbackIfPending(const char *reason);
}  // namespace ota
