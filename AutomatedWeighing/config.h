#pragma once
#include <stdint.h>
#include <stddef.h>

// --- pins (kept as macros to match original sketch idioms) ---
#define HX_DOUT 23
#define HX_SCK 22
#define I2C_SDA 18
#define I2C_SCL 21
#define BTN_SUCCESS 12
#define BTN_CANCEL 14
#define BUZZER_PIN 26

// --- network ---
extern const char* ssid;
extern const char* password;
extern const char* firebaseDatabaseUrl;
extern const char* firebaseScaleDeviceId;
extern const char* firebaseAuthToken;
extern const bool useFirebaseWorkerEsp32;

extern const char* ntpServer;
extern const long gmtOffset_sec;
extern const int daylightOffset_sec;

// --- display + EEPROM ---
extern const int LCD_COLS;
extern const int LCD_ROWS;
extern const int calVal_eepromAddress;

// --- calibration ---
extern const float OLD_DEFAULT_CALIBRATION_FACTOR;
extern const float PREVIOUS_CALIBRATION_FACTOR;
extern const float DEFAULT_CALIBRATION_FACTOR;
extern const float CALIBRATION_STEP;

// --- timing ---
extern const unsigned long DISPLAY_INTERVAL_MS;
extern const unsigned long SERIAL_DIAGNOSTIC_INTERVAL_MS;
extern const unsigned long BUTTON_DEBOUNCE_MS;
extern const unsigned long BUTTON_COOLDOWN_MS;
extern const unsigned long MESSAGE_DISPLAY_MS;
extern const unsigned long WIFI_RESULT_DISPLAY_MS;
extern const unsigned long TARE_TIMEOUT_MS;
extern const unsigned long PRICE_UPDATE_POLL_MS;
extern const unsigned long PRICE_TABLE_REFRESH_MS;
extern const unsigned long FIREBASE_HTTP_TIMEOUT_MS;
extern const unsigned long FIREBASE_RETRY_MS;
extern const unsigned long WORKER_SALE_RETRY_MS;
extern const unsigned long FIREBASE_READ_FAILURE_BACKOFF_MS;
extern const unsigned long FIREBASE_UPLOAD_FAILURE_BACKOFF_MS;
extern const unsigned int BUZZER_BEEP_MS;
extern const unsigned int BUZZER_PAUSE_MS;

// --- scale tuning ---
extern const float OBJECT_DETECT_GRAMS;
extern const float OBJECT_REMOVE_GRAMS;
extern const float NOISE_FLOOR_GRAMS;
extern const float WEIGHT_FILTER_ALPHA;
extern const float FAST_WEIGHT_FILTER_ALPHA;
extern const float FAST_WEIGHT_DELTA_GRAMS;
extern const float LOCK_STABLE_TOLERANCE_GRAMS;
extern const uint8_t OBJECT_CONFIRM_SAMPLES;
extern const uint8_t REMOVE_CONFIRM_SAMPLES;
extern const uint8_t LOCK_MATCH_SAMPLES;

// --- sales / camera ---
constexpr size_t SALE_HISTORY_SIZE = 10;  // compile-time so it can size arrays
extern const float FRUIT_DETECTION_CONFIDENCE;
extern const unsigned long CAMERA_START_DELAY_MS;
extern const unsigned long CAMERA_DETECTION_TIMEOUT_MS;
