#pragma once
#include <stdint.h>
#include <stddef.h>

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
