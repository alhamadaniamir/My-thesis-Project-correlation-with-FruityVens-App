#include "firebase_client.h"
#include "config.h"
#include "json_utils.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>

static uint32_t firebase_read_backoff_until_ms = 0;
static uint32_t firebase_upload_backoff_until_ms = 0;

static bool timedBackoffActive(uint32_t until_ms) {
  return until_ms != 0 && static_cast<int32_t>(until_ms - millis()) > 0;
}

bool firebaseReadBackoffActive() { return timedBackoffActive(firebase_read_backoff_until_ms); }
bool firebaseUploadBackoffActive() { return timedBackoffActive(firebase_upload_backoff_until_ms); }

void noteFirebaseReadFailure(int statusCode) {
  firebase_read_backoff_until_ms = millis() + kFirebaseReadFailureBackoffMs;
  Serial.print("Firebase read paused ");
  Serial.print(kFirebaseReadFailureBackoffMs / 1000);
  Serial.print("s");
  if (statusCode != 0) { Serial.print(" after HTTP "); Serial.print(statusCode); }
  Serial.println();
}

void noteFirebaseUploadFailure(int statusCode) {
  firebase_upload_backoff_until_ms = millis() + kFirebaseUploadFailureBackoffMs;
  Serial.print("Firebase upload paused ");
  Serial.print(kFirebaseUploadFailureBackoffMs / 1000);
  Serial.print("s");
  if (statusCode != 0) { Serial.print(" after HTTP "); Serial.print(statusCode); }
  Serial.println();
}

String firebaseBaseUrl() {
  String base = kFirebaseDatabaseUrl;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base;
}

String firebaseUrlForPath(const String& path) {
  String url = firebaseBaseUrl();
  if (!path.startsWith("/")) url += "/";
  url += path;
  url += ".json";
  if (strlen(kFirebaseAuthToken) > 0) {
    url += "?auth=";
    url += kFirebaseAuthToken;
  }
  return url;
}

String firebaseSaleKey(const SaleSyncPacket& sale) {
  if (strlen(sale.firebaseKey) > 0) return firebaseSafeKey(sale.firebaseKey);
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
  body += "  \"id\": "; body += String(sale.saleId);
  body += ",\n  \"weightGrams\": "; body += String(sale.weightGrams, 2);
  body += ",\n  \"weight\": "; body += String(sale.weightGrams, 2);
  body += ",\n  \"weightKg\": "; body += String(sale.weightGrams / 1000.0f, 3);
  body += ",\n  \"price\": "; body += String(sale.price, 2);
  body += ",\n  \"priceCentavos\": "; body += String(static_cast<int>(round(sale.price * 100.0f)));
  body += ",\n  \"pricePerKg\": "; body += String(sale.pricePerKg, 2);
  body += ",\n  \"pricePerKgCentavos\": "; body += String(static_cast<int>(round(sale.pricePerKg * 100.0f)));
  body += ",\n  \"fruitType\": \""; body += jsonEscape(sale.fruitType);
  body += "\",\n  \"fruit\": \""; body += jsonEscape(sale.fruitType);
  body += "\",\n  \"fruit_type\": \""; body += jsonEscape(sale.fruitType);
  body += "\",\n  \"timestamp\": \""; body += jsonEscape(sale.timestamp);
  body += "\",\n  \"soldAt\": \""; body += jsonEscape(sale.timestamp);
  body += "\",\n  \"date\": \""; body += jsonEscape(sale.date);
  body += "\",\n  \"time\": \""; body += jsonEscape(sale.time);
  body += "\",\n  \"source\": \""; body += jsonEscape(sale.source);
  body += "\",\n  \"sourceDeviceId\": \""; body += jsonEscape(kFirebaseScaleDeviceId);
  body += "\",\n  \"status\": \"sold\",\n  \"imported\": false,\n  \"createdAtMs\": ";
  body += String(sale.createdAtMs);
  body += "\n}";
  return body;
}

static WiFiClientSecure& sharedTlsClient() {
  static WiFiClientSecure client;
  static bool configured = false;
  if (!configured) {
    client.setInsecure();
    configured = true;
  }
  return client;
}

bool getFirebaseJson(const String& path, String& payload) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure& client = sharedTlsClient();

  HTTPClient http;
  http.setReuse(true);
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

bool uploadSaleToFirebase(const SaleSyncPacket& sale) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure& client = sharedTlsClient();

  HTTPClient http;
  http.setReuse(true);
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
