#include "json_utils.h"

String jsonEscape(const char* value) {
  String escaped = "";
  for (size_t i = 0; value[i] != '\0'; i++) {
    char c = value[i];
    if (c == '"' || c == '\\') { escaped += '\\'; escaped += c; }
    else if (c == '\n') escaped += "\\n";
    else if (c == '\r') escaped += "\\r";
    else if (c == '\t') escaped += "\\t";
    else escaped += c;
  }
  return escaped;
}

String firebaseSafeKey(const char* value) {
  String key = "";
  for (size_t i = 0; value[i] != '\0'; i++) {
    const char c = value[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_') {
      key += c;
    } else {
      key += '_';
    }
  }
  return key.length() == 0 ? "unknown" : key;
}

String firebaseSafeKey(const String& value) {
  char buffer[96];
  value.toCharArray(buffer, sizeof(buffer));
  return firebaseSafeKey(buffer);
}

int jsonValueStart(const String& json, const char* key, int searchFrom) {
  String pattern = "\"";
  pattern += key;
  pattern += "\"";
  int keyIndex = json.indexOf(pattern, searchFrom);
  if (keyIndex < 0) return -1;
  int colonIndex = json.indexOf(':', keyIndex + pattern.length());
  if (colonIndex < 0) return -1;
  int valueStart = colonIndex + 1;
  while (valueStart < (int)json.length()) {
    char c = json.charAt(valueStart);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
    valueStart++;
  }
  return valueStart;
}

bool extractJsonString(const String& json, const char* key, String& value, int searchFrom) {
  int start = jsonValueStart(json, key, searchFrom);
  if (start < 0 || start >= (int)json.length() || json.charAt(start) != '"') return false;
  String parsed = "";
  bool escaped = false;
  for (int i = start + 1; i < (int)json.length(); i++) {
    char c = json.charAt(i);
    if (escaped) { parsed += c; escaped = false; }
    else if (c == '\\') escaped = true;
    else if (c == '"') { value = parsed; return true; }
    else parsed += c;
  }
  return false;
}

static bool isJsonNumberChar(char c) {
  return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
}

bool extractJsonNumber(const String& json, const char* key, String& value, int searchFrom) {
  int start = jsonValueStart(json, key, searchFrom);
  if (start < 0 || start >= (int)json.length()) return false;
  int end = start;
  while (end < (int)json.length() && isJsonNumberChar(json.charAt(end))) end++;
  if (end == start) return false;
  value = json.substring(start, end);
  return true;
}
