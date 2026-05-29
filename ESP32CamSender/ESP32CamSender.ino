#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "protocol.h"
#include "config.h"
#include "camera_io.h"
#include "gemini_client.h"
#include "web_ui.h"

namespace {

uint8_t kBroadcastPeer[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint32_t sequence_id = 0;
uint32_t detection_started_ms = 0;
uint32_t last_preview_ms = 0;
TaskHandle_t identify_task_handle = nullptr;

char latest_label[32] = "Idle";
char latest_monitor_line[160] = "Waiting for scale...";
float latest_confidence = 0.0f;
uint32_t latest_uptime_ms = 0;
bool detection_active = false;
bool detection_inflight = false;
bool detection_result_sent = false;
bool preview_active = false;
bool camera_ready = false;

String macToString(const uint8_t* mac) {
  char buffer[18];
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buffer);
}

bool cameraOutputActive() { return detection_active || preview_active; }

void updateCameraOutput() {
  digitalWrite(kLedPin, cameraOutputActive() ? HIGH : LOW);
}

void setDetectionActive(bool active, const char* reason) {
  if (detection_active == active) return;
  detection_active = active;
  updateCameraOutput();
  if (active) {
    detection_started_ms = millis();
    detection_inflight = false;
    detection_result_sent = false;
    latest_confidence = 0.0f;
    latest_uptime_ms = millis();
    strlcpy(latest_label, "Settling", sizeof(latest_label));
    strlcpy(latest_monitor_line, "Detection started: settling", sizeof(latest_monitor_line));
  } else if (!detection_result_sent) {
    strlcpy(latest_label, "Idle", sizeof(latest_label));
    latest_confidence = 0.0f;
    latest_uptime_ms = millis();
    strlcpy(latest_monitor_line, "Detection stopped", sizeof(latest_monitor_line));
  }
  Serial.print(active ? "Detection ON: " : "Detection OFF: ");
  Serial.println(reason);
}

void setPreviewActive(bool active, const char* reason) {
  if (active) last_preview_ms = millis();
  if (preview_active == active) return;
  preview_active = active;
  updateCameraOutput();
  Serial.print(active ? "Preview ON: " : "Preview OFF: ");
  Serial.println(reason);
}

void sendDetection(const char* label, float confidence) {
  DetectionPacket packet = {};
  packet.packetType = kPacketTypeDetectionResult;
  strlcpy(packet.label, label, sizeof(packet.label));
  packet.confidence = confidence;
  packet.sequence = ++sequence_id;
  packet.uptime_ms = millis();

  snprintf(latest_monitor_line, sizeof(latest_monitor_line),
           "Sent: %s | conf=%.2f | seq=%lu",
           packet.label, packet.confidence,
           static_cast<unsigned long>(packet.sequence));
  strlcpy(latest_label, label, sizeof(latest_label));
  latest_confidence = confidence;
  latest_uptime_ms = millis();

  Serial.print("Sending detection: ");
  Serial.print(label);
  Serial.print(" conf=");
  Serial.println(confidence, 2);

  esp_err_t r = esp_now_send(kBroadcastPeer, reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
  if (r != ESP_OK) Serial.printf("esp_now_send() failed: %d\n", r);

  detection_result_sent = true;
  detection_inflight = false;
  setDetectionActive(false, "result sent");
}

void runIdentifyAndBroadcast() {
  uint8_t* jpg = nullptr;
  size_t jpg_len = 0;
  if (!captureRoiJpeg(&jpg, &jpg_len, kCaptureJpegQuality)) {
    sendDetection("Unknown", 0.0f);
    return;
  }
  Serial.print("Captured JPEG bytes: ");
  Serial.println(jpg_len);

  char label[32];
  const bool ok = geminiIdentify(jpg, jpg_len, label, sizeof(label));
  free(jpg);
  sendDetection(ok ? label : "Unknown", ok ? 1.0f : 0.0f);
}

void onSendComplete(const esp_now_send_info_t*, esp_now_send_status_t status) {
  Serial.print("ESP-NOW send -> ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void onPacketReceived(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len) {
  if (len != sizeof(ScaleCommandPacket)) return;
  ScaleCommandPacket packet = {};
  memcpy(&packet, data, sizeof(packet));
  packet.command[sizeof(packet.command) - 1] = '\0';
  if (packet.packetType != kPacketTypeScaleCommand) return;

  Serial.print("Scale command ");
  Serial.print(packet.command);
  Serial.print(" from ");
  Serial.println(macToString(recv_info->src_addr));

  if (strcmp(packet.command, "START") == 0) {
    setDetectionActive(true, "scale weight detected");
  } else if (strcmp(packet.command, "STOP") == 0) {
    setDetectionActive(false, "scale stopped");
  }
}

bool addBroadcastPeer() {
  if (esp_now_is_peer_exist(kBroadcastPeer)) return true;
  uint8_t channel = WiFi.channel();
  if (channel == 0) {
    channel = 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcastPeer, 6);
  peer.channel = channel;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("esp_now_add_peer(broadcast) failed");
    return false;
  }
  return true;
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPassword);
  Serial.print("Connecting WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi FAILED");
    WiFi.disconnect(false, false);
    return false;
  }
  Serial.print("WiFi OK. IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("WiFi channel: ");
  Serial.println(WiFi.channel());
  return true;
}

bool initEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init() failed");
    return false;
  }
  esp_now_register_send_cb(onSendComplete);
  esp_now_register_recv_cb(onPacketReceived);
  if (!addBroadcastPeer()) return false;
  Serial.print("Cam MAC: ");
  Serial.println(WiFi.macAddress());
  return true;
}

}  // namespace

// --- web_ui hooks ---

void onWebPreviewStart() { setPreviewActive(true, "web"); }
void onWebPreviewStop()  { setPreviewActive(false, "web"); }
void onWebDetectStart()  { setDetectionActive(true, "web test"); }
void onWebDetectStop()   { setDetectionActive(false, "web test"); }

CamUiStatus getCamUiStatus() {
  static char ip_buf[24];
  strlcpy(ip_buf, WiFi.localIP().toString().c_str(), sizeof(ip_buf));
  return CamUiStatus{
    latest_label,
    latest_monitor_line,
    latest_confidence,
    sequence_id,
    latest_uptime_ms,
    detection_active,
    preview_active,
    cameraOutputActive(),
    ip_buf,
  };
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println(kDeviceName);

  pinMode(kLedPin, OUTPUT);
  digitalWrite(kLedPin, LOW);

  if (!connectWifi()) {
    Serial.println("WiFi required for Gemini backend.");
  }

  if (!initEspNow()) {
    while (true) delay(1000);
  }

  if (!initCamera()) {
    while (true) delay(1000);
  }
  camera_ready = true;

  startWebServer();
  setDetectionActive(false, "startup idle");
}

void loop() {
  handleWebClient();

  if (preview_active && millis() - last_preview_ms >= kPreviewIdleTimeoutMs) {
    setPreviewActive(false, "preview idle timeout");
  }

  if (!detection_active) {
    delay(10);
    return;
  }

  if (millis() - detection_started_ms >= kDetectionTimeoutMs) {
    Serial.println("Detection timed out");
    sendDetection("Unknown", 0.0f);
    return;
  }

  if (millis() - detection_started_ms < kPlacementSettleMs) {
    strlcpy(latest_label, "Settling", sizeof(latest_label));
    delay(20);
    return;
  }

  if (detection_inflight || detection_result_sent || identify_task_handle != nullptr) {
    delay(20);
    return;
  }

  detection_inflight = true;
  strlcpy(latest_label, "Identifying", sizeof(latest_label));
  strlcpy(latest_monitor_line, "Calling Gemini backend...", sizeof(latest_monitor_line));

  identify_task_handle = nullptr;
  xTaskCreatePinnedToCore(
    [](void*) {
      runIdentifyAndBroadcast();
      identify_task_handle = nullptr;
      vTaskDelete(nullptr);
    },
    "identify", 8192, nullptr, 1, &identify_task_handle, 0);
}
