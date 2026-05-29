#include "gemini_client.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "mbedtls/base64.h"

static bool hasWord(const char* label, const char* word) {
  char lo[64]; size_t i;
  for (i = 0; i < sizeof(lo) - 1 && label[i]; i++)
    lo[i] = tolower((unsigned char)label[i]);
  lo[i] = '\0';
  char lw[32];
  for (i = 0; i < sizeof(lw) - 1 && word[i]; i++)
    lw[i] = tolower((unsigned char)word[i]);
  lw[i] = '\0';
  return strstr(lo, lw) != nullptr;
}

const char* normalizeFruitName(const char* raw) {
  if (raw == nullptr || raw[0] == '\0') return "Unknown";
  if (hasWord(raw, "apple"))                            return "Apple";
  if (hasWord(raw, "banana"))                           return "Banana";
  if (hasWord(raw, "mango"))                            return "Mango";
  if (hasWord(raw, "orange") || hasWord(raw, "mandarin") ||
      hasWord(raw, "tangerine") || hasWord(raw, "clementine")) return "Orange";
  if (hasWord(raw, "grape"))                            return "Grapes";
  if (hasWord(raw, "lemon") || hasWord(raw, "lime"))    return "Lemon";
  if (hasWord(raw, "strawberr"))                        return "Strawberries";
  if (hasWord(raw, "watermelon"))                       return "Watermelon";
  if (hasWord(raw, "pineapple"))                        return "Pineapple";
  if (hasWord(raw, "papaya"))                           return "Papaya";
  if (hasWord(raw, "pomelo"))                           return "Pomelo";
  if (hasWord(raw, "avocado"))                          return "Avocado";
  if (hasWord(raw, "guava"))                            return "Guava";
  if (hasWord(raw, "rambutan"))                         return "Rambutan";
  if (hasWord(raw, "pear"))                             return "Pear";
  if (hasWord(raw, "peach"))                            return "Peach";
  if (hasWord(raw, "coconut"))                          return "Coconut";
  if (hasWord(raw, "durian"))                           return "Durian";
  if (hasWord(raw, "jackfruit"))                        return "Jackfruit";
  if (hasWord(raw, "dragonfruit") || hasWord(raw, "dragon fruit")) return "Dragonfruit";
  return "Unknown";
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
  fruit.toCharArray(raw_buf, sizeof(raw_buf));
  const char* normalized = normalizeFruitName(raw_buf);
  strlcpy(out_label, normalized, out_size);
  Serial.print("Gemini: ");
  Serial.print(raw_buf);
  Serial.print(" -> ");
  Serial.println(normalized);
  return true;
}
