#include "OtaServer.h"

#include <Update.h>
#include <esp_ota_ops.h>

#include "config.h"

// Arduino-ESP32 hook (esp32-hal-misc.c): defer the OTA verification that
// initArduino() would otherwise perform immediately at boot.
extern "C" bool verifyRollbackLater() { return true; }

namespace {

constexpr char kRealm[] = "ESP32-CAM OTA";
constexpr char kPage[] =
    "<!DOCTYPE html><html><head><title>ESP32-CAM OTA</title></head><body>"
    "<h1>Firmware update</h1>"
    "<form method=\"POST\" action=\"/upload\" enctype=\"multipart/form-data\">"
    "<input type=\"file\" name=\"firmware\" accept=\".bin\"> <input type=\"submit\" value=\"Upload\">"
    "</form></body></html>";

bool pendingVerify() {
  esp_ota_img_states_t state;
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

}  // namespace

namespace ota {

void confirmImageIfPending() {
  if (!pendingVerify()) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    log_i("new firmware confirmed, rollback cancelled");
  } else {
    log_e("could not confirm the running firmware");
  }
}

void rollbackIfPending(const char *reason) {
  if (!pendingVerify()) return;
  log_e("new firmware failed (%s): rolling back", reason);
  esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return on success
}

}  // namespace ota

OtaServer::OtaServer(std::function<void()> stopStreaming)
    : server_(cfg::kOtaPort), stopStreaming_(std::move(stopStreaming)) {}

void OtaServer::begin() {
  server_.on("/", HTTP_GET, [this] { handleRoot(); });
  server_.on(
      "/upload", HTTP_POST, [this] { handleUploadDone(); }, [this] { handleUploadChunk(); });
  server_.onNotFound([this] { server_.send(404, "text/plain", "Not found"); });
  server_.begin();
  log_i("OTA server on port %u", unsigned(cfg::kOtaPort));
}

void OtaServer::poll() { server_.handleClient(); }

bool OtaServer::authorized() { return server_.authenticate(OTA_USERNAME, OTA_PASSWORD); }

void OtaServer::handleRoot() {
  if (!authorized()) {
    server_.requestAuthentication(BASIC_AUTH, kRealm);
    return;
  }
  server_.send(200, "text/html", kPage);
}

void OtaServer::handleUploadChunk() {
  HTTPUpload &upload = server_.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START:
      rejected_ = !authorized();
      if (rejected_) {
        log_w("OTA upload rejected: bad credentials");
        return;
      }
      log_i("OTA upload started: %s", upload.filename.c_str());
      updating_ = true;
      written_ = 0;
      if (stopStreaming_) stopStreaming_();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) log_e("Update.begin: %s", Update.errorString());
      break;
    case UPLOAD_FILE_WRITE:
      if (rejected_ || !Update.isRunning()) return;
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        log_e("Update.write: %s", Update.errorString());
        Update.abort();
      }
      written_ += upload.currentSize;
      break;
    case UPLOAD_FILE_END:
      if (rejected_ || !Update.isRunning()) return;
      if (!Update.end(true)) log_e("Update.end: %s", Update.errorString());
      break;
    case UPLOAD_FILE_ABORTED:
      if (Update.isRunning()) Update.abort();
      log_w("OTA upload aborted by the client");
      if (updating_) {
        delay(100);
        ESP.restart();  // streaming was stopped for the update: start over cleanly
      }
      break;
  }
}

void OtaServer::handleUploadDone() {
  if (rejected_ || !authorized()) {
    rejected_ = false;
    server_.requestAuthentication(BASIC_AUTH, kRealm);
    return;
  }
  const bool ok = updating_ && Update.isFinished() && !Update.hasError();
  if (ok) {
    log_i("OTA success (%u bytes), rebooting", unsigned(written_));
    server_.send(200, "text/plain", "Update successful, rebooting\n");
  } else {
    const String reason = updating_ ? String(Update.errorString()) : String("no firmware file received");
    log_e("OTA failed: %s", reason.c_str());
    server_.send(500, "text/plain", "Update failed: " + reason + "\n");
  }
  if (!updating_) return;  // streaming was never stopped: nothing to recover
  // Streaming was stopped for the update: reboot either way, into the new
  // image on success or back into the current one on failure.
  server_.client().flush();
  delay(500);
  ESP.restart();
}
