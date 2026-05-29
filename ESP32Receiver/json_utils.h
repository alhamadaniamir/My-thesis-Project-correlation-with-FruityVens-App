#pragma once
#include <Arduino.h>
#include <stdint.h>

String jsonEscape(const char* value);
String firebaseSafeKey(const char* value);

int jsonValueStart(const String& json, const char* key, int searchFrom = 0);
bool extractJsonString(const String& json, const char* key, String& value, int searchFrom = 0);
bool extractJsonNumber(const String& json, const char* key, String& value, int searchFrom = 0);
uint64_t parseUint64(const String& value);
