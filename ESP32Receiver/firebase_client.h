#pragma once
#include <Arduino.h>
#include "protocol.h"

// Backoff state and helpers.
bool firebaseReadBackoffActive();
bool firebaseUploadBackoffActive();
void noteFirebaseReadFailure(int statusCode = 0);
void noteFirebaseUploadFailure(int statusCode = 0);

// URL helpers (use the device id + database URL from config.h).
String firebaseBaseUrl();
String firebaseUrlForPath(const String& path);
String firebaseSaleKey(const SaleSyncPacket& sale);
String firebaseRequestUrl(const SaleSyncPacket& sale);
String saleRecordJson(const SaleSyncPacket& sale);

// HTTPS calls (return false on failure; populate read backoff/upload backoff).
bool getFirebaseJson(const String& path, String& payload);
bool uploadSaleToFirebase(const SaleSyncPacket& sale);
