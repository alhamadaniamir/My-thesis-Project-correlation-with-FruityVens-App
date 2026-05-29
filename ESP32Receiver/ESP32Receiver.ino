#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "config.h"
#include "firebase_client.h"
#include "json_utils.h"

namespace {

uint8_t kBroadcastPeer[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct FruitPriceVersion {
  char fruitType[32];
  uint64_t version;
};

SaleSyncPacket sale_queue[kSaleQueueSize] = {};
FruitPriceVersion price_versions[kPriceVersionCacheSize] = {};
size_t sale_queue_count = 0;
size_t price_version_count = 0;
uint32_t worker_sequence = 0;
uint32_t last_price_update_poll_ms = 0;
uint32_t last_price_table_refresh_ms = 0;
uint32_t last_upload_retry_ms = 0;
volatile bool sale_upload_due_now = false;
String last_price_update_version = "";

String macToString(const uint8_t* mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

const char* canonicalFruitType(const char* fruitType) {
  if (fruitType == nullptr || strlen(fruitType) == 0) return "";
  if (strcmp(fruitType, "Grape") == 0 ||
      strcmp(fruitType, "Grape Blue") == 0 ||
      strcmp(fruitType, "Grape Pink") == 0 ||
      strcmp(fruitType, "Grape White") == 0) return "Grapes";
  if (strcmp(fruitType, "Strawberry") == 0) return "Strawberries";
  if (strcmp(fruitType, "Mango Carabao") == 0 ||
      strcmp(fruitType, "Indian Mango") == 0 ||
      strcmp(fruitType, "Mango Red") == 0) return "Mango";
  if (strcmp(fruitType, "Lime") == 0 || strcmp(fruitType, "Limes") == 0) return "Lemon";
  if (strcmp(fruitType, "Mandarine") == 0) return "Orange";
  if (strcmp(fruitType, "Pomelo Sweetie") == 0) return "Pomelo";
  return fruitType;
}

bool rememberPriceVersion(const char* fruitType, uint64_t version) {
  if (version == 0) return true;
  for (size_t i = 0; i < price_version_count; i++) {
    if (strcmp(price_versions[i].fruitType, fruitType) != 0) continue;
    if (version < price_versions[i].version) return false;
    price_versions[i].version = version;
    return true;
  }
  if (price_version_count >= kPriceVersionCacheSize) return true;
  strlcpy(price_versions[price_version_count].fruitType, fruitType,
          sizeof(price_versions[price_version_count].fruitType));
  price_versions[price_version_count].version = version;
  price_version_count++;
  return true;
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPassword);
  Serial.print("Connecting WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    WiFi.disconnect(false, false);
    return false;
  }
  Serial.println("WiFi connected");
  Serial.print("WiFi channel: ");
  Serial.println(WiFi.channel());
  return true;
}

bool addEspNowBroadcastPeer() {
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
    Serial.println("Failed to add ESP-NOW broadcast peer");
    return false;
  }
  return true;
}

void sendSaleAck(const char* firebaseKey, bool accepted) {
  SaleAckPacket packet = {};
  packet.packetType = kPacketTypeSaleAck;
  packet.sequence = ++worker_sequence;
  packet.accepted = accepted;
  strlcpy(packet.firebaseKey, firebaseKey, sizeof(packet.firebaseKey));
  esp_err_t r = esp_now_send(kBroadcastPeer, reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
  if (r != ESP_OK) Serial.printf("Sale ack send failed: %d\n", r);
}

void sendPriceUpdate(const char* fruitType, float pricePerKg) {
  PriceUpdatePacket packet = {};
  packet.packetType = kPacketTypePriceUpdate;
  packet.sequence = ++worker_sequence;
  packet.pricePerKg = pricePerKg;
  strlcpy(packet.fruitType, fruitType, sizeof(packet.fruitType));
  esp_err_t r = esp_now_send(kBroadcastPeer, reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
  if (r != ESP_OK) Serial.printf("Price update send failed: %d\n", r);
}

bool saleQueued(const char* firebaseKey) {
  for (size_t i = 0; i < sale_queue_count; i++) {
    if (strcmp(sale_queue[i].firebaseKey, firebaseKey) == 0) return true;
  }
  return false;
}

void queueSale(const SaleSyncPacket& incoming) {
  SaleSyncPacket sale = incoming;
  sale.fruitType[sizeof(sale.fruitType) - 1] = '\0';
  sale.timestamp[sizeof(sale.timestamp) - 1] = '\0';
  sale.date[sizeof(sale.date) - 1] = '\0';
  sale.time[sizeof(sale.time) - 1] = '\0';
  sale.source[sizeof(sale.source) - 1] = '\0';
  sale.firebaseKey[sizeof(sale.firebaseKey) - 1] = '\0';

  if (saleQueued(sale.firebaseKey)) { sendSaleAck(sale.firebaseKey, true); return; }
  if (sale_queue_count >= kSaleQueueSize) {
    Serial.println("Sale queue full; rejected sale packet");
    sendSaleAck(sale.firebaseKey, false);
    return;
  }
  sale_queue[sale_queue_count++] = sale;
  sendSaleAck(sale.firebaseKey, true);
  Serial.print("Queued sale ");
  Serial.print(sale.firebaseKey);
  Serial.print(", pending: ");
  Serial.println(sale_queue_count);
  sale_upload_due_now = true;
}

void maintainSaleUploads(bool force) {
  if (WiFi.status() != WL_CONNECTED || sale_queue_count == 0 || firebaseUploadBackoffActive()) return;
  if (!force && millis() - last_upload_retry_ms < kFirebaseUploadRetryMs) return;
  last_upload_retry_ms = millis();
  if (!uploadSaleToFirebase(sale_queue[0])) return;
  for (size_t i = 1; i < sale_queue_count; i++) sale_queue[i - 1] = sale_queue[i];
  sale_queue_count--;
  sale_queue[sale_queue_count] = SaleSyncPacket{};
  Serial.print("Worker uploaded sale, remaining: ");
  Serial.println(sale_queue_count);
}

void maintainQueuedSaleUploads() {
  const bool force_upload = sale_upload_due_now;
  sale_upload_due_now = false;
  maintainSaleUploads(force_upload);
}

bool applyPriceObject(const String& json, int searchFrom = 0) {
  String fruit;
  if (!extractJsonString(json, "fruit", fruit, searchFrom)) return false;

  String priceCentavosText;
  float price = 0.0f;
  if (extractJsonNumber(json, "priceCentavos", priceCentavosText, searchFrom)) {
    price = priceCentavosText.toInt() / 100.0f;
  } else {
    String priceText;
    if (extractJsonNumber(json, "price", priceText, searchFrom)) price = priceText.toFloat();
  }
  if (fruit.length() == 0 || price <= 0.0f) return false;

  char fruit_buf[32];
  fruit.toCharArray(fruit_buf, sizeof(fruit_buf));
  const char* scale_fruit = canonicalFruitType(fruit_buf);

  uint64_t version = 0;
  String version_text;
  if (extractJsonNumber(json, "version", version_text, searchFrom)) {
    version = parseUint64(version_text);
  }
  if (!rememberPriceVersion(scale_fruit, version)) {
    Serial.print("Skipped stale price: ");
    Serial.print(scale_fruit);
    Serial.print(" from ");
    Serial.println(fruit_buf);
    return false;
  }
  sendPriceUpdate(scale_fruit, price);
  Serial.print("Broadcast price: ");
  Serial.print(scale_fruit);
  if (strcmp(scale_fruit, fruit_buf) != 0) {
    Serial.print(" from "); Serial.print(fruit_buf);
  }
  Serial.print(" = PHP "); Serial.print(price, 2); Serial.println("/kg");
  return true;
}

bool applyPriceTablePayload(const String& payload) {
  if (payload.length() == 0 || payload == "null") return false;
  bool updated_any = false;
  int search_from = 0;
  while (true) {
    int fruit_index = payload.indexOf("\"fruit\"", search_from);
    if (fruit_index < 0) break;
    if (applyPriceObject(payload, fruit_index)) updated_any = true;
    search_from = fruit_index + 7;
  }
  return updated_any;
}

bool fetchPriceTable() {
  String path = "scalePriceUpdates/";
  path += firebaseSafeKey(kFirebaseScaleDeviceId);
  path += "/fruits";
  String payload;
  if (!getFirebaseJson(path, payload)) return false;
  return applyPriceTablePayload(payload);
}

bool fetchLatestPriceUpdate() {
  String path = "scalePriceUpdates/";
  path += firebaseSafeKey(kFirebaseScaleDeviceId);
  path += "/latest";
  String payload;
  if (!getFirebaseJson(path, payload) || payload.length() == 0 || payload == "null") return false;
  String version;
  if (!extractJsonNumber(payload, "version", version)) version = payload;
  if (version == last_price_update_version) return true;
  const bool applied_latest = applyPriceObject(payload);
  last_price_update_version = version;
  fetchPriceTable();
  return applied_latest;
}

void maintainPriceUpdates() {
  if (WiFi.status() != WL_CONNECTED || firebaseReadBackoffActive()) return;
  const uint32_t now = millis();
  if (last_price_table_refresh_ms == 0 || now - last_price_table_refresh_ms >= kPriceTableRefreshMs) {
    last_price_table_refresh_ms = now;
    fetchPriceTable();
    if (firebaseReadBackoffActive()) return;
  }
  if (now - last_price_update_poll_ms >= kPriceUpdatePollMs) {
    last_price_update_poll_ms = now;
    fetchLatestPriceUpdate();
  }
}

void onPacketReceived(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len) {
  if (len == sizeof(SaleSyncPacket)) {
    SaleSyncPacket packet = {};
    memcpy(&packet, data, sizeof(packet));
    if (packet.packetType == kPacketTypeSaleSync) { queueSale(packet); return; }
  }
  if (len == sizeof(ScaleCommandPacket) || len == sizeof(DetectionPacket)) return;  // ignore peers' broadcasts
  Serial.print("Ignored ESP-NOW packet from ");
  Serial.print(macToString(recv_info->src_addr));
  Serial.print(" size=");
  Serial.println(len);
}

bool initEspNow() {
  const bool wifi_connected = connectWifi();
  uint8_t channel = WiFi.channel();
  if (channel == 0) channel = 1;
  if (!wifi_connected && esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("Failed to set WiFi channel");
    return false;
  }
  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init() failed");
    return false;
  }
  esp_now_register_recv_cb(onPacketReceived);
  if (!addEspNowBroadcastPeer()) return false;
  Serial.print("Worker MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("ESP-NOW channel: ");
  Serial.println(channel);
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
    while (true) delay(1000);
  }
}

void loop() {
  maintainQueuedSaleUploads();
  maintainPriceUpdates();
  delay(10);
}
