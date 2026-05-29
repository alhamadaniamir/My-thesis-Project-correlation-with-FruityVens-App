#pragma once
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// POST a JPEG to the Rust backend and return the normalized fruit name.
// Returns true on success, populating `out_label` (use a 32-byte buffer).
bool geminiIdentify(const uint8_t* jpg, size_t jpg_len, char* out_label, size_t out_size);

// Collapse free-form Gemini responses into the master's price-table keys.
const char* normalizeFruitName(const char* raw);

// Base64-encode a buffer; caller frees the returned pointer with free().
char* base64EncodeAlloc(const uint8_t* src, size_t src_len);

// Pull a string value from a flat JSON document.
bool extractJsonString(const String& json, const char* key, String& value);
