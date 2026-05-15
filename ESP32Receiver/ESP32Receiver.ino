#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

namespace {

constexpr char kDeviceName[] = "ESP32 Receiver";
constexpr uint8_t kEspNowChannel = 1;
constexpr int kStatusLedPin = 2;
constexpr float kDetectionThreshold = 0.60f;
constexpr char kLowConfidencePrefix[] = "[LOW CONFIDENCE] ";
constexpr uint32_t kNoPacketMessageIntervalMs = 3000;

// Optional filter. Set to false to accept packets from any ESP32-CAM sender.
constexpr bool kFilterSender = false;
uint8_t kExpectedSenderMac[] = {0x24, 0x6F, 0x28, 0x00, 0x00, 0x02};

struct DetectionPacket {
  char label[32];
  float confidence;
  uint32_t sequence;
  uint32_t uptime_ms;
};

uint32_t last_packet_ms = 0;
uint32_t last_idle_message_ms = 0;

String macToString(const uint8_t* mac) {
  char buffer[18];
  snprintf(
    buffer,
    sizeof(buffer),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
  return String(buffer);
}

bool macMatches(const uint8_t* lhs, const uint8_t* rhs) {
  for (int i = 0; i < 6; ++i) {
    if (lhs[i] != rhs[i]) {
      return false;
    }
  }
  return true;
}

void onPacketReceived(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len) {
  if (len != sizeof(DetectionPacket)) {
    Serial.printf("Ignored packet with unexpected size: %d\n", len);
    return;
  }

  const uint8_t* sender_mac = recv_info->src_addr;
  if (kFilterSender && !macMatches(sender_mac, kExpectedSenderMac)) {
    Serial.print("Ignored sender ");
    Serial.println(macToString(sender_mac));
    return;
  }

  DetectionPacket packet = {};
  memcpy(&packet, data, sizeof(packet));
  packet.label[sizeof(packet.label) - 1] = '\0';
  last_packet_ms = millis();
  char display_label[sizeof(packet.label) + sizeof(kLowConfidencePrefix)] = {};

  if (packet.confidence >= kDetectionThreshold) {
    strlcpy(display_label, packet.label, sizeof(display_label));
  } else {
    snprintf(
      display_label,
      sizeof(display_label),
      "%s%s",
      kLowConfidencePrefix,
      packet.label
    );
  }

  digitalWrite(kStatusLedPin, HIGH);

  Serial.print("From ");
  Serial.print(macToString(sender_mac));
  Serial.print(" | seq=");
  Serial.print(packet.sequence);
  Serial.print(" | label=");
  Serial.print(display_label);
  Serial.print(" | confidence=");
  Serial.print(packet.confidence, 4);
  Serial.print(" | sender_uptime_ms=");
  Serial.println(packet.uptime_ms);

  digitalWrite(kStatusLedPin, LOW);
}

bool initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);

  if (esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("Failed to set WiFi channel");
    return false;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init() failed");
    return false;
  }

  esp_now_register_recv_cb(onPacketReceived);

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("ESP-NOW channel: ");
  Serial.println(kEspNowChannel);
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println(kDeviceName);

  pinMode(kStatusLedPin, OUTPUT);
  digitalWrite(kStatusLedPin, LOW);

  if (!initEspNow()) {
    while (true) {
      delay(1000);
    }
  }
}

void loop() {
  uint32_t now = millis();
  if (now - last_idle_message_ms >= kNoPacketMessageIntervalMs) {
    if (last_packet_ms == 0) {
      Serial.println("Waiting for sender...");
    } else if (now - last_packet_ms >= kNoPacketMessageIntervalMs) {
      Serial.println("No packet received recently");
    }
    last_idle_message_ms = now;
  }

  delay(100);
}
