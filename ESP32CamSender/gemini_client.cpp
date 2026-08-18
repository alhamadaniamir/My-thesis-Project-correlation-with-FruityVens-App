#include "gemini_client.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ctype.h>
#include <string.h>
#include "mbedtls/base64.h"

struct FruitAlias {
  const char* alias;
  const char* label;
};

static const FruitAlias kAllowedFruitAliases[] = {
  {"Apple", "Apple"},
  {"Mango Carabao", "Mango Carabao"},
  {"Carabao Mango", "Mango Carabao"},
  {"Indian Mango", "Indian Mango"},
  {"Apple Mango", "Apple Mango"},
  {"Dragon Fruit", "Dragon Fruit"},
  {"Dragonfruit", "Dragon Fruit"},
  {"Watermelon", "Watermelon"},
  {"Pineapple", "Pineapple"},
  {"Mangosteen", "Mangosteen"},
  {"Banana", "Banana"},
  {"Avocado", "Avocado"},
  {"Grapes", "Grapes"},
  {"Grape", "Grapes"},
  {"Durian", "Durian"},
  {"Pomelo", "Pomelo"},
  {"Pomelo Sweetie", "Pomelo"},
  {"Pear", "Pear"},
  {"Lemon", "Lemon"},
  {"Lime", "Lemon"},
  {"Limes", "Lemon"},
  {"Orange", "Orange"},
  {"Mandarin", "Orange"},
  {"Mandarine", "Orange"},
};

static void comparableFruitNameCopy(const char* raw, char* out, size_t out_size) {
  if (out_size == 0) return;
  size_t j = 0;
  for (size_t i = 0; raw[i] && j < out_size - 1; i++) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    if (!isalnum(c)) continue;
    out[j++] = static_cast<char>(tolower(c));
  }
  out[j] = '\0';
}

static bool fruitNameEquals(const char* a, const char* b) {
  char ca[48];
  char cb[48];
  comparableFruitNameCopy(a, ca, sizeof(ca));
  comparableFruitNameCopy(b, cb, sizeof(cb));
  return strcmp(ca, cb) == 0;
}

static const char* allowedFruitLabelFor(const char* raw) {
  for (size_t i = 0; i < sizeof(kAllowedFruitAliases) / sizeof(kAllowedFruitAliases[0]); i++) {
    if (fruitNameEquals(raw, kAllowedFruitAliases[i].alias)) {
      return kAllowedFruitAliases[i].label;
    }
  }
  return nullptr;
}

char* base64EncodeAlloc(const uint8_t* src, size_t src_len) {
  size_t out_size = 0;
  mbedtls_base64_encode(nullptr, 0, &out_size, src, src_len);
  if (out_size == 0) return nullptr;
  char* out = static_cast<char*>(psramFound() ? ps_malloc(out_size + 1) : malloc(out_size + 1));
  if (out == nullptr) return nullptr;
  size_t written = 0;
  if (mbedtls_base64_encode(
        reinterpret_cast<unsigned char*>(out),
        out_size, &written, src, src_len) != 0) {
    free(out);
    return nullptr;
  }
  out[written] = '\0';
  return out;
}

bool extractJsonString(const String& json, const char* key, String& value) {
  String pattern = "\"";
  pattern += key;
  pattern += "\"";
  int k = json.indexOf(pattern);
  if (k < 0) return false;
  int colon = json.indexOf(':', k + pattern.length());
  if (colon < 0) return false;
  int start = colon + 1;
  while (start < (int)json.length() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\n' || json[start] == '\r')) start++;
  if (start >= (int)json.length() || json[start] != '"') return false;
  String parsed = "";
  bool escaped = false;
  for (int i = start + 1; i < (int)json.length(); i++) {
    char c = json[i];
    if (escaped) { parsed += c; escaped = false; }
    else if (c == '\\') escaped = true;
    else if (c == '"') { value = parsed; return true; }
    else parsed += c;
  }
  return false;
}

bool geminiIdentify(const uint8_t* jpg, size_t jpg_len, char* out_label, size_t out_size) {
  if (out_size == 0) return false;
  strlcpy(out_label, "Unknown", out_size);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Identify aborted: WiFi down");
    return false;
  }

  char* b64 = base64EncodeAlloc(jpg, jpg_len);
  if (b64 == nullptr) {
    Serial.println("base64 alloc failed");
    return false;
  }

  String body;
  body.reserve(strlen(b64) + 64);
  body = "{\"mime_type\":\"image/jpeg\",\"data\":\"";
  body += b64;
  body += "\"}";
  free(b64);

  static WiFiClientSecure client;
  static bool tls_configured = false;
  if (!tls_configured) {
    client.setInsecure();
    tls_configured = true;
  }

  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(kBackendHttpTimeoutMs);
  if (!http.begin(client, kBackendUrl)) {
    Serial.println("HTTP begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  String auth = "Bearer ";
  auth += kBackendAuthToken;
  http.addHeader("Authorization", auth);

  const int code = http.POST(body);
  const String response = http.getString();
  http.end();

  Serial.print("Backend HTTP ");
  Serial.print(code);
  Serial.print(" body: ");
  Serial.println(response);

  if (code < 200 || code >= 300) return false;

  String fruit;
  if (!extractJsonString(response, "fruit", fruit) || fruit.length() == 0) return false;

  char raw_buf[64];
  fruit.trim();
  fruit.toCharArray(raw_buf, sizeof(raw_buf));
  const char* allowed_label = allowedFruitLabelFor(raw_buf);
  if (allowed_label == nullptr) {
    Serial.print("Rejected unsupported fruit from Gemini: ");
    Serial.println(raw_buf);
    return false;
  }

  strlcpy(out_label, allowed_label, out_size);
  Serial.print("Gemini: ");
  Serial.print(raw_buf);
  Serial.print(" -> ");
  Serial.println(out_label);
  return true;
}
