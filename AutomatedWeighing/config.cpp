#include "config.h"

const char* ssid = "Aida_iPhone";
const char* password = "1234567899";

const char* firebaseDatabaseUrl = "https://fruityv-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* firebaseScaleDeviceId = "fruityvens-scale-01";
const char* firebaseAuthToken = "";
const bool useFirebaseWorkerEsp32 = true;

const char* ntpServer = "time.google.com";
const long gmtOffset_sec = 8 * 3600;
const int daylightOffset_sec = 0;

const int LCD_COLS = 20;
const int LCD_ROWS = 4;
const int calVal_eepromAddress = 0;

const float OLD_DEFAULT_CALIBRATION_FACTOR = 1.0f;
const float PREVIOUS_CALIBRATION_FACTOR = 0.1012f;
const float DEFAULT_CALIBRATION_FACTOR = 218.306412f;
const float CALIBRATION_STEP = 1.0f;

const unsigned long DISPLAY_INTERVAL_MS = 300;
const unsigned long SERIAL_DIAGNOSTIC_INTERVAL_MS = 500;
const unsigned long BUTTON_DEBOUNCE_MS = 40;
const unsigned long BUTTON_COOLDOWN_MS = 300;
const unsigned long MESSAGE_DISPLAY_MS = 1000;
const unsigned long WIFI_RESULT_DISPLAY_MS = 2000;
const unsigned long TARE_TIMEOUT_MS = 5000;
const unsigned long PRICE_UPDATE_POLL_MS = 5000;
const unsigned long PRICE_TABLE_REFRESH_MS = 60000;
const unsigned long FIREBASE_HTTP_TIMEOUT_MS = 1500;
const unsigned long FIREBASE_RETRY_MS = 5000;
const unsigned long WORKER_SALE_RETRY_MS = 1000;
const unsigned long FIREBASE_READ_FAILURE_BACKOFF_MS = 60000;
const unsigned long FIREBASE_UPLOAD_FAILURE_BACKOFF_MS = 15000;
const unsigned int BUZZER_BEEP_MS = 220;
const unsigned int BUZZER_PAUSE_MS = 120;

const float OBJECT_DETECT_GRAMS = 9.0f;
const float OBJECT_REMOVE_GRAMS = 4.0f;
const float NOISE_FLOOR_GRAMS = 2.0f;
const float WEIGHT_FILTER_ALPHA = 0.45f;
const float FAST_WEIGHT_FILTER_ALPHA = 0.85f;
const float FAST_WEIGHT_DELTA_GRAMS = 20.0f;
const float LOCK_STABLE_TOLERANCE_GRAMS = 1.0f;
const uint8_t OBJECT_CONFIRM_SAMPLES = 3;
const uint8_t REMOVE_CONFIRM_SAMPLES = 3;
const uint8_t LOCK_MATCH_SAMPLES = 10;
const unsigned long OBJECT_REDETECT_COOLDOWN_MS = 2000;

const float FRUIT_DETECTION_CONFIDENCE = 0.0f;
const unsigned long CAMERA_START_DELAY_MS = 1000;
// Must exceed the camera's worst case (settle 1.2s + Gemini HTTP up to 15s,
// hard-capped at 20s on the ESP32-CAM). If the scale times out first it shows
// "Unknown" and the late camera result then overwrites it, causing the LCD to
// flash Identifying -> Unknown -> <fruit>. Wait longer than the camera's 20s.
const unsigned long CAMERA_DETECTION_TIMEOUT_MS = 22000;
// How many times to re-send START (waiting another timeout window each time)
// before giving up and showing "Unknown". This only matters if a packet is
// dropped or the camera is offline; a healthy camera replies on the first try.
const uint8_t CAMERA_DETECTION_MAX_RETRIES = 3;
