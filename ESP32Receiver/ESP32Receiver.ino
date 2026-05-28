#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr char kDeviceName[] = "ESP32 Firebase Worker";
constexpr char kWifiSsid[] = "DITO_3CFF6_2.4";
constexpr char kWifiPassword[] = "48b84252";
constexpr char kFirebaseDatabaseUrl[] = "https://fruityv-default-rtdb.asia-southeast1.firebasedatabase.app";
constexpr char kFirebaseScaleDeviceId[] = "fruityvens-scale-01";
constexpr char kFirebaseAuthToken[] = "";
constexpr int kStatusLedPin = 2;
constexpr uint32_t kPriceUpdatePollMs = 5000;
constexpr uint32_t kPriceTableRefreshMs = 60000;
constexpr uint32_t kFirebaseHttpTimeoutMs = 1500;
constexpr uint32_t kFirebaseReadFailureBackoffMs = 60000;
constexpr uint32_t kFirebaseUploadRetryMs = 500;
constexpr uint32_t kFirebaseUploadFailureBackoffMs = 15000;
constexpr size_t kSaleQueueSize = 10;
constexpr size_t kPriceVersionCacheSize = 40;
constexpr uint8_t kPacketTypeScaleCommand = 1;
constexpr uint8_t kPacketTypeDetectionResult = 2;
constexpr uint8_t kPacketTypeSaleSync = 3;
constexpr uint8_t kPacketTypePriceUpdate = 4;
constexpr uint8_t kPacketTypeSaleAck = 5;

uint8_t kBroadcastPeer[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct ScaleCommandPacket {
  uint8_t packetType;
  char command[16];
  uint32_t sequence;
  uint32_t uptime_ms;
};

struct DetectionPacket {
  uint8_t packetType;
  char label[32];
  float confidence;
  uint32_t sequence;
  uint32_t uptime_ms;
};

struct SaleSyncPacket {
  uint8_t packetType;
  uint32_t sequence;
  uint32_t saleId;
  uint32_t createdAtMs;
  float weightGrams;
  float price;
  float pricePerKg;
  char fruitType[32];
  char timestamp[25];
  char date[11];
  char time[9];
  char source[16];
  char firebaseKey[80];
};

struct PriceUpdatePacket {
  uint8_t packetType;
  uint32_t sequence;
  float pricePerKg;
  char fruitType[32];
};

struct SaleAckPacket {
  uint8_t packetType;
  uint32_t sequence;
  bool accepted;
  char firebaseKey[80];
};

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
uint32_t firebase_read_backoff_until_ms = 0;
uint32_t firebase_upload_backoff_until_ms = 0;
volatile bool sale_upload_due_now = false;
String last_price_update_version = "";

void maintainSaleUploads(bool force = false);

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

bool timedBackoffActive(uint32_t until_ms) {
  return until_ms != 0 && static_cast<int32_t>(until_ms - millis()) > 0;
}

bool firebaseReadBackoffActive() {
  return timedBackoffActive(firebase_read_backoff_until_ms);
}

bool firebaseUploadBackoffActive() {
  return timedBackoffActive(firebase_upload_backoff_until_ms);
}

void noteFirebaseReadFailure(int statusCode = 0) {
  firebase_read_backoff_until_ms = millis() + kFirebaseReadFailureBackoffMs;
  Serial.print("Firebase read paused ");
  Serial.print(kFirebaseReadFailureBackoffMs / 1000);
  Serial.print("s");
  if (statusCode != 0) {
    Serial.print(" after HTTP ");
    Serial.print(statusCode);
  }
  Serial.println();
}

void noteFirebaseUploadFailure(int statusCode = 0) {
  firebase_upload_backoff_until_ms = millis() + kFirebaseUploadFailureBackoffMs;
  Serial.print("Firebase upload paused ");
  Serial.print(kFirebaseUploadFailureBackoffMs / 1000);
  Serial.print("s");
  if (statusCode != 0) {
    Serial.print(" after HTTP ");
    Serial.print(statusCode);
  }
  Serial.println();
}

String jsonEscape(const char* value) {
  String escaped = "";
  for (size_t i = 0; value[i] != '\0'; i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      escaped += '\\';
      escaped += c;
    } else if (c == '\n') {
      escaped += "\\n";
    } else if (c == '\r') {
      escaped += "\\r";
    } else if (c == '\t') {
      escaped += "\\t";
    } else {
      escaped += c;
    }
  }
  return escaped;
}

String firebaseSafeKey(const char* value) {
  String key = "";
  for (size_t i = 0; value[i] != '\0'; i++) {
    const char c = value[i];
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_') {
      key += c;
    } else {
      key += '_';
    }
  }
  return key.length() == 0 ? "unknown" : key;
}

const char* canonicalFruitType(const char* fruitType) {
  if (fruitType == nullptr || strlen(fruitType) == 0) {
    return "";
  }

  if (strcmp(fruitType, "Grape") == 0 ||
      strcmp(fruitType, "Grape Blue") == 0 ||
      strcmp(fruitType, "Grape Pink") == 0 ||
      strcmp(fruitType, "Grape White") == 0) {
    return "Grapes";
  }
  if (strcmp(fruitType, "Strawberry") == 0) {
    return "Strawberries";
  }
  if (strcmp(fruitType, "Mango Carabao") == 0 ||
      strcmp(fruitType, "Indian Mango") == 0 ||
      strcmp(fruitType, "Mango Red") == 0) {
    return "Mango";
  }
  if (strcmp(fruitType, "Lime") == 0 || strcmp(fruitType, "Limes") == 0) {
    return "Lemon";
  }
  if (strcmp(fruitType, "Mandarine") == 0) {
    return "Orange";
  }
  if (strcmp(fruitType, "Pomelo Sweetie") == 0) {
    return "Pomelo";
  }

  return fruitType;
}

String firebaseBaseUrl() {
  String base = kFirebaseDatabaseUrl;
  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  return base;
}

String firebaseUrlForPath(const String& path) {
  String url = firebaseBaseUrl();
  if (!path.startsWith("/")) {
    url += "/";
  }
  url += path;
  url += ".json";
  if (strlen(kFirebaseAuthToken) > 0) {
    url += "?auth=";
    url += kFirebaseAuthToken;
  }
  return url;
}

String firebaseSaleKey(const SaleSyncPacket& sale) {
  if (strlen(sale.firebaseKey) > 0) {
    return firebaseSafeKey(sale.firebaseKey);
  }
  String key = String(sale.saleId);
  key += "_";
  key += sale.timestamp;
  key += "_";
  key += String(sale.createdAtMs);
  return firebaseSafeKey(key.c_str());
}

String firebaseRequestUrl(const SaleSyncPacket& sale) {
  String path = "scaleSales/";
  path += firebaseSafeKey(kFirebaseScaleDeviceId);
  path += "/";
  path += firebaseSaleKey(sale);
  return firebaseUrlForPath(path);
}

String saleRecordJson(const SaleSyncPacket& sale) {
  String body = "{\n";
  body += "  \"id\": ";
  body += String(sale.saleId);
  body += ",\n  \"weightGrams\": ";
  body += String(sale.weightGrams, 2);
  body += ",\n  \"weight\": ";
  body += String(sale.weightGrams, 2);
  body += ",\n  \"weightKg\": ";
  body += String(sale.weightGrams / 1000.0f, 3);
  body += ",\n  \"price\": ";
  body += String(sale.price, 2);
  body += ",\n  \"priceCentavos\": ";
  body += String(static_cast<int>(round(sale.price * 100.0f)));
  body += ",\n  \"pricePerKg\": ";
  body += String(sale.pricePerKg, 2);
  body += ",\n  \"pricePerKgCentavos\": ";
  body += String(static_cast<int>(round(sale.pricePerKg * 100.0f)));
  body += ",\n  \"fruitType\": \"";
  body += jsonEscape(sale.fruitType);
  body += "\",\n  \"fruit\": \"";
  body += jsonEscape(sale.fruitType);
  body += "\",\n  \"fruit_type\": \"";
  body += jsonEscape(sale.fruitType);
  body += "\",\n  \"timestamp\": \"";
  body += jsonEscape(sale.timestamp);
  body += "\",\n  \"soldAt\": \"";
  body += jsonEscape(sale.timestamp);
  body += "\",\n  \"date\": \"";
  body += jsonEscape(sale.date);
  body += "\",\n  \"time\": \"";
  body += jsonEscape(sale.time);
  body += "\",\n  \"source\": \"";
  body += jsonEscape(sale.source);
  body += "\",\n  \"sourceDeviceId\": \"";
  body += jsonEscape(kFirebaseScaleDeviceId);
  body += "\",\n  \"status\": \"sold\",\n  \"imported\": false,\n  \"createdAtMs\": ";
  body += String(sale.createdAtMs);
  body += "\n}";
  return body;
}

int jsonValueStart(const String& json, const char* key, int searchFrom = 0) {
  String pattern = "\"";
  pattern += key;
  pattern += "\"";
  int keyIndex = json.indexOf(pattern, searchFrom);
  if (keyIndex < 0) {
    return -1;
  }

  int colonIndex = json.indexOf(':', keyIndex + pattern.length());
  if (colonIndex < 0) {
    return -1;
  }

  int valueStart = colonIndex + 1;
  while (valueStart < json.length()) {
    char c = json.charAt(valueStart);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    valueStart++;
  }
  return valueStart;
}

bool extractJsonString(const String& json, const char* key, String& value, int searchFrom = 0) {
  int start = jsonValueStart(json, key, searchFrom);
  if (start < 0 || start >= json.length() || json.charAt(start) != '"') {
    return false;
  }

  String parsed = "";
  bool escaped = false;
  for (int i = start + 1; i < json.length(); i++) {
    char c = json.charAt(i);
    if (escaped) {
      parsed += c;
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      value = parsed;
      return true;
    } else {
      parsed += c;
    }
  }
  return false;
}

bool isJsonNumberChar(char c) {
  return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
}

bool extractJsonNumber(const String& json, const char* key, String& value, int searchFrom = 0) {
  int start = jsonValueStart(json, key, searchFrom);
  if (start < 0 || start >= json.length()) {
    return false;
  }

  int end = start;
  while (end < json.length() && isJsonNumberChar(json.charAt(end))) {
    end++;
  }

  if (end == start) {
    return false;
  }

  value = json.substring(start, end);
  return true;
}

uint64_t parseUint64(const String& value) {
  char buffer[24];
  value.toCharArray(buffer, sizeof(buffer));
  return strtoull(buffer, nullptr, 10);
}

bool getFirebaseJson(const String& path, String& payload) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(kFirebaseHttpTimeoutMs);
  const String url = firebaseUrlForPath(path);
  if (!http.begin(client, url)) {
    Serial.println("Firebase read failed: could not start HTTPS request");
    noteFirebaseReadFailure();
    return false;
  }

  const int statusCode = http.GET();
  payload = http.getString();
  http.end();

  if (statusCode < 200 || statusCode >= 300) {
    Serial.print("Firebase read HTTP ");
    Serial.print(statusCode);
    Serial.print(" path=");
    Serial.print(path);
    Serial.print(" response=");
    Serial.println(payload);
    noteFirebaseReadFailure(statusCode);
    return false;
  }

  return true;
}

bool rememberPriceVersion(const char* fruitType, uint64_t version) {
  if (version == 0) {
    return true;
  }

  for (size_t i = 0; i < price_version_count; i++) {
    if (strcmp(price_versions[i].fruitType, fruitType) != 0) {
      continue;
    }
    if (version < price_versions[i].version) {
      return false;
    }
    price_versions[i].version = version;
    return true;
  }

  if (price_version_count >= kPriceVersionCacheSize) {
    return true;
  }

  strlcpy(
    price_versions[price_version_count].fruitType,
    fruitType,
    sizeof(price_versions[price_version_count].fruitType)
  );
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
  if (esp_now_is_peer_exist(kBroadcastPeer)) {
    return true;
  }

  uint8_t channel = WiFi.channel();
  if (channel == 0) {
    channel = 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  }

  esp_now_peer_info_t peer_info = {};
  memcpy(peer_info.peer_addr, kBroadcastPeer, 6);
  peer_info.channel = channel;
  peer_info.encrypt = false;

  if (esp_now_add_peer(&peer_info) != ESP_OK) {
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

  esp_err_t result = esp_now_send(
    kBroadcastPeer,
    reinterpret_cast<uint8_t*>(&packet),
    sizeof(packet)
  );
  if (result != ESP_OK) {
    Serial.printf("Sale ack send failed: %d\n", result);
  }
}

void sendPriceUpdate(const char* fruitType, float pricePerKg) {
  PriceUpdatePacket packet = {};
  packet.packetType = kPacketTypePriceUpdate;
  packet.sequence = ++worker_sequence;
  packet.pricePerKg = pricePerKg;
  strlcpy(packet.fruitType, fruitType, sizeof(packet.fruitType));

  esp_err_t result = esp_now_send(
    kBroadcastPeer,
    reinterpret_cast<uint8_t*>(&packet),
    sizeof(packet)
  );
  if (result != ESP_OK) {
    Serial.printf("Price update send failed: %d\n", result);
  }
}

bool saleQueued(const char* firebaseKey) {
  for (size_t i = 0; i < sale_queue_count; i++) {
    if (strcmp(sale_queue[i].firebaseKey, firebaseKey) == 0) {
      return true;
    }
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

  if (saleQueued(sale.firebaseKey)) {
    sendSaleAck(sale.firebaseKey, true);
    return;
  }

  if (sale_queue_count >= kSaleQueueSize) {
    Serial.println("Sale queue full; rejected sale packet");
    sendSaleAck(sale.firebaseKey, false);
    return;
  }

  sale_queue[sale_queue_count++] = sale;
  sendSaleAck(sale.firebaseKey, true);

  Serial.print("Queued sale ");
  Serial.print(sale.firebaseKey);
  Serial.print(", pending worker uploads: ");
  Serial.println(sale_queue_count);

  sale_upload_due_now = true;
}

bool uploadSaleToFirebase(const SaleSyncPacket& sale) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(kFirebaseHttpTimeoutMs);
  const String url = firebaseRequestUrl(sale);
  if (!http.begin(client, url)) {
    Serial.println("Firebase upload failed: could not start HTTPS request");
    noteFirebaseUploadFailure();
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  const int statusCode = http.PUT(saleRecordJson(sale));
  http.end();

  Serial.print("Firebase upload HTTP ");
  Serial.println(statusCode);
  if (statusCode < 200 || statusCode >= 300) {
    noteFirebaseUploadFailure(statusCode);
    return false;
  }

  return true;
}

void maintainSaleUploads(bool force) {
  if (WiFi.status() != WL_CONNECTED ||
      sale_queue_count == 0 ||
      firebaseUploadBackoffActive()) {
    return;
  }

  if (!force && millis() - last_upload_retry_ms < kFirebaseUploadRetryMs) {
    return;
  }

  last_upload_retry_ms = millis();
  if (!uploadSaleToFirebase(sale_queue[0])) {
    return;
  }

  for (size_t i = 1; i < sale_queue_count; i++) {
    sale_queue[i - 1] = sale_queue[i];
  }
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
  if (!extractJsonString(json, "fruit", fruit, searchFrom)) {
    return false;
  }

  String priceCentavosText;
  float price = 0.0f;
  if (extractJsonNumber(json, "priceCentavos", priceCentavosText, searchFrom)) {
    price = priceCentavosText.toInt() / 100.0f;
  } else {
    String priceText;
    if (extractJsonNumber(json, "price", priceText, searchFrom)) {
      price = priceText.toFloat();
    }
  }

  if (fruit.length() == 0 || price <= 0.0f) {
    return false;
  }

  char fruit_buffer[32];
  fruit.toCharArray(fruit_buffer, sizeof(fruit_buffer));
  const char* scale_fruit = canonicalFruitType(fruit_buffer);

  uint64_t version = 0;
  String version_text;
  if (extractJsonNumber(json, "version", version_text, searchFrom)) {
    version = parseUint64(version_text);
  }
  if (!rememberPriceVersion(scale_fruit, version)) {
    Serial.print("Skipped stale price: ");
    Serial.print(scale_fruit);
    Serial.print(" from ");
    Serial.println(fruit_buffer);
    return false;
  }

  sendPriceUpdate(scale_fruit, price);
  Serial.print("Broadcast price: ");
  Serial.print(scale_fruit);
  if (strcmp(scale_fruit, fruit_buffer) != 0) {
    Serial.print(" from ");
    Serial.print(fruit_buffer);
  }
  Serial.print(" = PHP ");
  Serial.print(price, 2);
  Serial.println("/kg");
  return true;
}

bool applyPriceTablePayload(const String& payload) {
  if (payload.length() == 0 || payload == "null") {
    return false;
  }

  bool updated_any = false;
  int search_from = 0;
  while (true) {
    int fruit_index = payload.indexOf("\"fruit\"", search_from);
    if (fruit_index < 0) {
      break;
    }
    if (applyPriceObject(payload, fruit_index)) {
      updated_any = true;
    }
    search_from = fruit_index + 7;
  }
  return updated_any;
}

bool fetchPriceTable() {
  String path = "scalePriceUpdates/";
  path += firebaseSafeKey(kFirebaseScaleDeviceId);
  path += "/fruits";

  String payload;
  if (!getFirebaseJson(path, payload)) {
    return false;
  }

  return applyPriceTablePayload(payload);
}

bool fetchLatestPriceUpdate() {
  String path = "scalePriceUpdates/";
  path += firebaseSafeKey(kFirebaseScaleDeviceId);
  path += "/latest";

  String payload;
  if (!getFirebaseJson(path, payload) || payload.length() == 0 || payload == "null") {
    return false;
  }

  String version;
  if (!extractJsonNumber(payload, "version", version)) {
    version = payload;
  }

  if (version == last_price_update_version) {
    return true;
  }

  const bool applied_latest = applyPriceObject(payload);
  last_price_update_version = version;
  fetchPriceTable();
  return applied_latest;
}

void maintainPriceUpdates() {
  if (WiFi.status() != WL_CONNECTED || firebaseReadBackoffActive()) {
    return;
  }

  const uint32_t now = millis();
  if (last_price_table_refresh_ms == 0 ||
      now - last_price_table_refresh_ms >= kPriceTableRefreshMs) {
    last_price_table_refresh_ms = now;
    fetchPriceTable();
    if (firebaseReadBackoffActive()) {
      return;
    }
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
    if (packet.packetType == kPacketTypeSaleSync) {
      queueSale(packet);
      return;
    }
  }

  if (len == sizeof(ScaleCommandPacket)) {
    ScaleCommandPacket command = {};
    memcpy(&command, data, sizeof(command));
    command.command[sizeof(command.command) - 1] = '\0';
    if (command.packetType == kPacketTypeScaleCommand) {
      return;
    }
  }

  if (len == sizeof(DetectionPacket)) {
    DetectionPacket packet = {};
    memcpy(&packet, data, sizeof(packet));
    packet.label[sizeof(packet.label) - 1] = '\0';
    if (packet.packetType == kPacketTypeDetectionResult) {
      return;
    }
  }

  Serial.print("Ignored ESP-NOW packet from ");
  Serial.print(macToString(recv_info->src_addr));
  Serial.print(" size=");
  Serial.println(len);
}

bool initEspNow() {
  const bool wifi_connected = connectWifi();

  uint8_t channel = WiFi.channel();
  if (channel == 0) {
    channel = 1;
  }

  if (!wifi_connected && esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("Failed to set WiFi channel");
    return false;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init() failed");
    return false;
  }

  esp_now_register_recv_cb(onPacketReceived);
  if (!addEspNowBroadcastPeer()) {
    return false;
  }

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
    while (true) {
      delay(1000);
    }
  }
}

void loop() {
  maintainQueuedSaleUploads();
  maintainPriceUpdates();
  delay(10);
}
