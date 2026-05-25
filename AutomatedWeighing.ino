#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <HX711_ADC.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <RTClib.h>
#include "time.h"
#include <math.h>
#include <string.h>

#define HX_DOUT 23
#define HX_SCK 22

#define I2C_SDA 18
#define I2C_SCL 21

#define BTN_SUCCESS 12
#define BTN_CANCEL 14
#define BUZZER_PIN 26

const char* ssid = "Aida_iPhone";
const char* password = "1234567899";

const char* firebaseDatabaseUrl = "https://fruityv-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* firebaseScaleDeviceId = "fruityvens-scale-01";
const char* firebaseAuthToken = "";

const char* ntpServer = "time.google.com";
const long gmtOffset_sec = 8 * 3600;
const int daylightOffset_sec = 0;

const int LCD_COLS = 20;
const int LCD_ROWS = 4;
const int calVal_eepromAddress = 0;

const float OLD_DEFAULT_CALIBRATION_FACTOR = 1.0;
const float PREVIOUS_CALIBRATION_FACTOR = 0.1012;
const float DEFAULT_CALIBRATION_FACTOR = 0.0102;
const float CALIBRATION_STEP = 0.001;
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
const unsigned long FIREBASE_READ_FAILURE_BACKOFF_MS = 60000;
const unsigned long FIREBASE_UPLOAD_FAILURE_BACKOFF_MS = 15000;
const unsigned int BUZZER_BEEP_MS = 220;
const unsigned int BUZZER_PAUSE_MS = 120;
const float OBJECT_DETECT_GRAMS = 5.0;
const float OBJECT_REMOVE_GRAMS = 2.0;
const float NOISE_FLOOR_GRAMS = 2.0;
const float WEIGHT_FILTER_ALPHA = 0.45;
const float FAST_WEIGHT_FILTER_ALPHA = 0.85;
const float FAST_WEIGHT_DELTA_GRAMS = 20.0;
const float LOCK_STABLE_TOLERANCE_GRAMS = 1.0;
const uint8_t OBJECT_CONFIRM_SAMPLES = 1;
const uint8_t REMOVE_CONFIRM_SAMPLES = 2;
const uint8_t LOCK_MATCH_SAMPLES = 10;
const size_t SALE_HISTORY_SIZE = 10;
const float FRUIT_DETECTION_CONFIDENCE = 0.0;
const unsigned long CAMERA_START_DELAY_MS = 1000;
const unsigned long CAMERA_DETECTION_TIMEOUT_MS = 12000;
const uint8_t PACKET_TYPE_SCALE_COMMAND = 1;
const uint8_t PACKET_TYPE_DETECTION_RESULT = 2;

uint8_t broadcastPeer[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct ScaleCommandPacket {
  uint8_t packetType;
  char command[16];
  uint32_t sequence;
  uint32_t uptime_ms;
};

struct DetectionPacket {
  uint8_t packetType;
  char label[32];
  float confidence;
  uint32_t sequence;
  uint32_t uptime_ms;
};

struct FruitPrice {
  const char* fruitType;
  float pricePerKg;
};

struct SaleRecord {
  unsigned long id;
  float weightGrams;
  float price;
  float pricePerKg;
  char fruitType[32];
  char timestamp[25];
  char date[11];
  char time[9];
  char source[16];
  char firebaseKey[80];
  unsigned long createdAtMs;
};

FruitPrice fruitPrices[] = {
  {"Apple", 90.0},
  {"Orange", 85.0},
  {"Banana", 35.0},
  {"Mango", 60.0},
  {"Grapes", 130.0},
  {"Lemon", 70.0},
  {"Papaya", 50.0},
  {"Watermelon", 50.0},
  {"Pineapple", 45.0},
  {"Calamansi", 65.0},
  {"Pomelo", 95.0},
  {"Guava", 50.0},
  {"Avocado", 110.0},
  {"Coconut", 35.0},
  {"Dalandan", 75.0},
  {"Dragon Fruit", 140.0},
  {"Durian", 180.0},
  {"Mangosteen", 160.0},
  {"Rambutan", 120.0},
  {"Lanzones", 120.0},
  {"Chico", 80.0},
  {"Atis", 95.0},
  {"Santol", 70.0},
  {"Star Apple", 90.0},
  {"Jackfruit", 65.0},
  {"Tamarind", 75.0},
  {"Melon", 55.0},
  {"Guyabano", 100.0},
  {"Mango Carabao", 80.0},
  {"Indian Mango", 75.0},
  {"Langkatan", 45.0},
  {"Pear", 95.0},
  {"Strawberries", 120.0},
};

hd44780_I2Cexp lcd(0x27);
HX711_ADC LoadCell(HX_DOUT, HX_SCK);
RTC_DS3231 rtc;

struct ButtonState {
  uint8_t pin;
  bool rawPressed;
  bool stablePressed;
  bool previousStablePressed;
  bool armed;
  unsigned long lastRawChangeMs;
};

ButtonState successButton = { BTN_SUCCESS, false, false, false, false, 0 };
ButtonState cancelButton = { BTN_CANCEL, false, false, false, false, 0 };

float calibration_factor = DEFAULT_CALIBRATION_FACTOR;
float currentWeightGrams = 0.0;
unsigned long lastDisplayMs = 0;
unsigned long lastSerialDiagnosticMs = 0;
unsigned long lastHx711UpdateMs = 0;
unsigned long lastSuccessActionMs = 0;
unsigned long lastCancelActionMs = 0;
unsigned long messageUntilMs = 0;
float lastLockCandidateGrams = 0.0;
float lockWeightSumGrams = 0.0;
float lockedWeightGrams = 0.0;
float filteredWeightGrams = 0.0;
float lastSensorWeightGrams = 0.0;
float lastProcessedWeightGrams = 0.0;
float lastReturnedWeightGrams = 0.0;
float lastFilterAlpha = 0.0;
bool hx711Ready = false;
bool rtcReady = false;
bool weightLocked = false;
bool objectPresent = false;
bool newScaleData = false;
uint8_t objectDetectCount = 0;
uint8_t objectRemoveCount = 0;
uint8_t lockMatchCount = 0;
uint8_t lockWeightSampleCount = 0;
unsigned long nextSaleId = 1;
unsigned long latestSaleId = 0;
size_t saleHistoryCount = 0;
SaleRecord saleHistory[SALE_HISTORY_SIZE];
char currentFruitType[32] = "Unknown";
unsigned long lastFirebaseRetryMs = 0;
unsigned long lastPriceUpdatePollMs = 0;
unsigned long lastPriceTableRefreshMs = 0;
unsigned long firebaseReadBackoffUntilMs = 0;
unsigned long firebaseUploadBackoffUntilMs = 0;
String lastPriceUpdateVersion = "";
unsigned long objectPresentStartedMs = 0;
unsigned long cameraDetectionStartedMs = 0;
uint32_t cameraCommandSequence = 0;
bool espNowReady = false;
bool cameraDetectionRequested = false;
bool cameraResultReceived = false;
bool cameraScanAttemptedForCurrentObject = false;
bool saleConfirmedForCurrentObject = false;

SaleRecord recordSale(const char* source);
String saleRecordJson(const SaleRecord& sale);
String saleRecordJson(const SaleRecord& sale, const String& indent);
String firebaseSafeKey(const char* value);
String firebaseSafeKey(const String& value);
bool uploadPendingSales();
bool maintainPendingSaleUpload();
bool setupEspNow();
void requestFruitDetection();
void stopFruitDetection(bool clearFruit);
void maintainFruitDetection();
void maintainPriceUpdates();
bool fetchPriceTable();
bool fetchLatestPriceUpdate();
bool hasFruitPrice(const char* fruitType);
float pricePerKgForFruit(const char* fruitType);

float calculatePrice(float weightGrams) {
  if (weightGrams < 0) weightGrams = 0;
  return (weightGrams / 1000.0) * pricePerKgForFruit(currentFruitType);
}

float calculatePriceForFruit(const char* fruitType, float weightGrams) {
  if (weightGrams < 0) weightGrams = 0;
  return (weightGrams / 1000.0) * pricePerKgForFruit(fruitType);
}

const char* scaleStatusText() {
  if (weightLocked) return "locked";
  if (objectPresent) return "weighing";
  return "zero";
}

String jsonEscape(const char* value) {
  String escaped = "";
  for (size_t i = 0; value[i] != '\0'; i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      escaped += '\\';
      escaped += c;
    } else if (c == '\n') {
      escaped += "\\n";
    } else if (c == '\r') {
      escaped += "\\r";
    } else if (c == '\t') {
      escaped += "\\t";
    } else {
      escaped += c;
    }
  }
  return escaped;
}

bool isValidCalibrationFactor(float value) {
  return !isnan(value) && !isinf(value) && fabs(value) >= 0.0001 && fabs(value) <= 1000000.0;
}

void printPadded(uint8_t col, uint8_t row, const char* text) {
  lcd.setCursor(col, row);
  lcd.print(text);
  for (uint8_t i = strlen(text); i < LCD_COLS - col; i++) {
    lcd.print(' ');
  }
}

void formatDisplayWeight(float weightGrams, char* buffer, size_t bufferSize) {
  if (weightGrams < 0.0f) {
    weightGrams = 0.0f;
  }

  if (weightGrams < 1000.0f) {
    snprintf(buffer, bufferSize, "%.0fg", weightGrams);
    return;
  }

  const float weightKg = weightGrams / 1000.0f;
  snprintf(buffer, bufferSize, "%.2fkg", weightKg);
}

bool hasFruitPrice(const char* fruitType) {
  if (fruitType == nullptr || strlen(fruitType) == 0) {
    return false;
  }

  for (size_t i = 0; i < sizeof(fruitPrices) / sizeof(fruitPrices[0]); i++) {
    if (strcmp(fruitPrices[i].fruitType, fruitType) == 0 &&
        fruitPrices[i].pricePerKg > 0.0f) {
      return true;
    }
  }
  return false;
}

float pricePerKgForFruit(const char* fruitType) {
  if (fruitType == nullptr || strlen(fruitType) == 0) {
    return 0.0f;
  }

  for (size_t i = 0; i < sizeof(fruitPrices) / sizeof(fruitPrices[0]); i++) {
    if (strcmp(fruitPrices[i].fruitType, fruitType) == 0) {
      return fruitPrices[i].pricePerKg;
    }
  }
  return 0.0f;
}

bool setFruitPrice(const char* fruitType, float pricePerKg) {
  if (fruitType == nullptr || strlen(fruitType) == 0 || pricePerKg <= 0.0f) {
    return false;
  }

  for (size_t i = 0; i < sizeof(fruitPrices) / sizeof(fruitPrices[0]); i++) {
    if (strcmp(fruitPrices[i].fruitType, fruitType) == 0) {
      fruitPrices[i].pricePerKg = pricePerKg;
      Serial.print("Price updated: ");
      Serial.print(fruitType);
      Serial.print(" = PHP ");
      Serial.print(pricePerKg, 2);
      Serial.println("/kg");
      return true;
    }
  }

  Serial.print("Ignored price update for unmanaged fruit: ");
  Serial.println(fruitType);
  return false;
}

void beginButton(ButtonState &button) {
  button.rawPressed = digitalRead(button.pin) == LOW;
  button.stablePressed = button.rawPressed;
  button.previousStablePressed = button.stablePressed;
  button.armed = !button.stablePressed;
  button.lastRawChangeMs = millis();
}

bool updateButton(ButtonState &button, unsigned long nowMs) {
  bool rawPressed = digitalRead(button.pin) == LOW;

  if (rawPressed != button.rawPressed) {
    button.rawPressed = rawPressed;
    button.lastRawChangeMs = nowMs;
  }

  button.previousStablePressed = button.stablePressed;
  if (nowMs - button.lastRawChangeMs >= BUTTON_DEBOUNCE_MS) {
    button.stablePressed = button.rawPressed;
  }

  if (!button.stablePressed) {
    button.armed = true;
  }

  if (button.armed && button.stablePressed && !button.previousStablePressed) {
    button.armed = false;
    return true;
  }

  return false;
}

void beepBuzzer(uint8_t beepCount) {
  for (uint8_t i = 0; i < beepCount; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(BUZZER_BEEP_MS);
    digitalWrite(BUZZER_PIN, LOW);

    if (i + 1 < beepCount) {
      delay(BUZZER_PAUSE_MS);
    }
  }
}

void resetWeightState() {
  stopFruitDetection(true);
  weightLocked = false;
  objectPresent = false;
  cameraScanAttemptedForCurrentObject = false;
  saleConfirmedForCurrentObject = false;
  lockedWeightGrams = 0.0;
  currentWeightGrams = 0.0;
  lastLockCandidateGrams = 0.0;
  lockWeightSumGrams = 0.0;
  filteredWeightGrams = 0.0;
  lastSensorWeightGrams = 0.0;
  lastProcessedWeightGrams = 0.0;
  lastReturnedWeightGrams = 0.0;
  lastFilterAlpha = 0.0;
  objectDetectCount = 0;
  objectRemoveCount = 0;
  lockMatchCount = 0;
  lockWeightSampleCount = 0;
  objectPresentStartedMs = 0;
}

void persistCalibrationFactor(float value) {
  EEPROM.put(calVal_eepromAddress, value);
  EEPROM.commit();
}

void saveCalibrationFactor(float value) {
  if (!isValidCalibrationFactor(value)) {
    Serial.println("Invalid calibration factor, not saved.");
    return;
  }

  calibration_factor = value;
  LoadCell.setCalFactor(calibration_factor);
  persistCalibrationFactor(calibration_factor);
  Serial.print("Calibration factor saved: ");
  Serial.println(calibration_factor, 6);
}

void printScaleHelp() {
  Serial.println("Commands:");
  Serial.println("  t = tare / zero");
  Serial.println("  + or a = increase calibration factor");
  Serial.println("  - or z = decrease calibration factor");
  Serial.println("  r = reset calibration factor");
  Serial.println("  c 500 = calibrate with known 500g weight");
  Serial.println("  fruit Mango = set fruit name sent with Firebase sale");
  Serial.println("Serial Monitor: 115200 baud");
  Serial.println("Buttons: released=1, pressed=0");
}

float readWeightGrams() {
  if (!hx711Ready) {
    return currentWeightGrams;
  }

  float weight = LoadCell.getData();
  lastSensorWeightGrams = weight;
  if (isnan(weight) || isinf(weight)) {
    Serial.println("Invalid HX711 reading");
    return currentWeightGrams;
  }

  weight = fabs(weight);
  if (weight < NOISE_FLOOR_GRAMS) weight = 0;
  lastProcessedWeightGrams = weight;

  if (filteredWeightGrams == 0.0 && weight >= OBJECT_DETECT_GRAMS) {
    filteredWeightGrams = weight;
    lastFilterAlpha = 1.0;
    lastReturnedWeightGrams = round(filteredWeightGrams * 10.0) / 10.0;
    return lastReturnedWeightGrams;
  }

  float filterAlpha = WEIGHT_FILTER_ALPHA;
  if (fabs(weight - filteredWeightGrams) >= FAST_WEIGHT_DELTA_GRAMS) {
    filterAlpha = FAST_WEIGHT_FILTER_ALPHA;
  }
  lastFilterAlpha = filterAlpha;

  filteredWeightGrams =
    (filterAlpha * weight) + ((1.0 - filterAlpha) * filteredWeightGrams);
  if (filteredWeightGrams < NOISE_FLOOR_GRAMS) filteredWeightGrams = 0;

  lastReturnedWeightGrams = round(filteredWeightGrams * 10.0) / 10.0;
  return lastReturnedWeightGrams;
}

void printScaleDiagnostics() {
  const unsigned long nowMs = millis();
  Serial.print("SCALE status=");
  Serial.print(scaleStatusText());
  Serial.print(" sensor=");
  Serial.print(lastSensorWeightGrams, 2);
  Serial.print("g processed=");
  Serial.print(lastProcessedWeightGrams, 2);
  Serial.print("g filtered=");
  Serial.print(filteredWeightGrams, 2);
  Serial.print("g current=");
  Serial.print(currentWeightGrams, 2);
  Serial.print("g locked=");
  Serial.print(lockedWeightGrams, 2);
  Serial.print("g avg=");
  if (lockWeightSampleCount > 0) {
    Serial.print(lockWeightSumGrams / lockWeightSampleCount, 2);
  } else {
    Serial.print("0.00");
  }
  Serial.print("g filter=");
  Serial.print(lastFilterAlpha, 2);
  Serial.print(" lock=");
  Serial.print(static_cast<unsigned int>(lockMatchCount));
  Serial.print("/");
  Serial.print(static_cast<unsigned int>(LOCK_MATCH_SAMPLES));
  Serial.print(" tol=");
  Serial.print(LOCK_STABLE_TOLERANCE_GRAMS, 1);
  Serial.print("g");
  Serial.print(" fruit=");
  Serial.print(currentFruitType);
  Serial.print(" cal=");
  Serial.print(calibration_factor, 6);
  Serial.print(" btnOK=");
  Serial.print(digitalRead(BTN_SUCCESS));
  Serial.print(" btnCancel=");
  Serial.print(digitalRead(BTN_CANCEL));
  Serial.print(" hxAgeMs=");
  Serial.println(nowMs - lastHx711UpdateMs);
}

void showMessage(const char* line1, const char* line2 = "", unsigned long nowMs = millis()) {
  lcd.clear();
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  messageUntilMs = nowMs + MESSAGE_DISPLAY_MS;
}

void tareScale(const char* reason) {
  if (!hx711Ready) {
    showMessage("Scale not ready");
    return;
  }

  Serial.println(reason);
  showMessage("Taring scale...");
  LoadCell.tareNoDelay();
  unsigned long tareStartedMs = millis();
  bool tareComplete = false;
  while (!tareComplete && millis() - tareStartedMs < TARE_TIMEOUT_MS) {
    LoadCell.update();
    tareComplete = LoadCell.getTareStatus();
  }

  if (!tareComplete) {
    showMessage("Tare failed", "Check scale");
    Serial.println("Tare timeout - check scale stability and wiring");
    return;
  }

  resetWeightState();
  showMessage("Scale zeroed");
  Serial.println("Tare complete");
}

SaleRecord confirmSale(const char* reason, const char* source) {
  if (currentWeightGrams < OBJECT_DETECT_GRAMS) {
    showMessage("No item on scale");
    beepBuzzer(2);
    return SaleRecord{};
  }

  if (cameraDetectionRequested ||
      strcmp(currentFruitType, "Processing") == 0 ||
      strcmp(currentFruitType, "Settling") == 0 ||
      !cameraResultReceived) {
    showMessage("Detecting fruit", "Please wait");
    beepBuzzer(2);
    return SaleRecord{};
  }

  if (strcmp(currentFruitType, "Unknown") == 0) {
    showMessage("Check fruit", "No sale saved");
    beepBuzzer(2);
    return SaleRecord{};
  }

  if (saleConfirmedForCurrentObject) {
    showMessage("Already saved", "Remove item");
    beepBuzzer(2);
    return SaleRecord{};
  }

  if (!hasFruitPrice(currentFruitType)) {
    showMessage("No price set", currentFruitType);
    beepBuzzer(2);
    return SaleRecord{};
  }

  SaleRecord sale = recordSale(source);

  Serial.print(reason);
  Serial.print(" - Sale ID: ");
  Serial.print(sale.id);
  Serial.print(" Fruit: ");
  Serial.print(sale.fruitType);
  Serial.print(" - Weight(g): ");
  Serial.print(sale.weightGrams, 2);
  Serial.print(" Price: ");
  Serial.print(sale.price, 2);
  Serial.print(" Timestamp: ");
  Serial.println(sale.timestamp);
  Serial.print("SALE_DATA ");
  Serial.println(saleRecordJson(sale));

  char line2[21];
  snprintf(line2, sizeof(line2), "PHP %.2f", sale.price);
  showMessage("Sale confirmed", line2);
  lastFirebaseRetryMs = millis() - FIREBASE_RETRY_MS;
  saleConfirmedForCurrentObject = true;
  stopFruitDetection(false);
  beepBuzzer(1);
  return sale;
}

void cancelSale(const char* reason) {
  showMessage("Cancelled");
  beepBuzzer(2);
  tareScale(reason);
}

void updateLockedWeight() {
  if (!hx711Ready || !newScaleData) return;
  newScaleData = false;

  float liveWeightGrams = readWeightGrams();

  if (weightLocked) {
    currentWeightGrams = lockedWeightGrams;
    if (liveWeightGrams <= OBJECT_REMOVE_GRAMS) {
      if (objectRemoveCount < REMOVE_CONFIRM_SAMPLES) objectRemoveCount++;
    } else {
      objectRemoveCount = 0;
    }

    if (objectRemoveCount >= REMOVE_CONFIRM_SAMPLES) {
      resetWeightState();
      Serial.println("Object removed - zero display resumed");
      return;
    }

    if (!cameraResultReceived &&
        !cameraDetectionRequested &&
        !cameraScanAttemptedForCurrentObject &&
        objectPresentStartedMs != 0 &&
        millis() - objectPresentStartedMs >= CAMERA_START_DELAY_MS) {
      requestFruitDetection();
    }
    return;
  }

  if (liveWeightGrams < OBJECT_DETECT_GRAMS) {
    stopFruitDetection(true);
    objectPresent = false;
    cameraScanAttemptedForCurrentObject = false;
    saleConfirmedForCurrentObject = false;
    currentWeightGrams = 0.0;
    objectDetectCount = 0;
    objectRemoveCount = 0;
    lockMatchCount = 0;
    lockWeightSampleCount = 0;
    lastLockCandidateGrams = 0.0;
    lockWeightSumGrams = 0.0;
    objectPresentStartedMs = 0;
    return;
  }

  if (objectDetectCount < OBJECT_CONFIRM_SAMPLES) {
    objectDetectCount++;
    currentWeightGrams = 0.0;
    lockMatchCount = 1;
    lockWeightSampleCount = 1;
    lastLockCandidateGrams = liveWeightGrams;
    lockWeightSumGrams = liveWeightGrams;
    return;
  }

  if (!objectPresent) {
    objectPresentStartedMs = millis();
    strlcpy(currentFruitType, "Settling", sizeof(currentFruitType));
  }
  objectPresent = true;
  if (!cameraResultReceived &&
      !cameraScanAttemptedForCurrentObject &&
      millis() - objectPresentStartedMs >= CAMERA_START_DELAY_MS) {
    requestFruitDetection();
  }
  objectRemoveCount = 0;
  currentWeightGrams = liveWeightGrams;
  if (fabs(liveWeightGrams - lastLockCandidateGrams) <= LOCK_STABLE_TOLERANCE_GRAMS) {
    if (lockMatchCount < LOCK_MATCH_SAMPLES) {
      lockMatchCount++;
      lockWeightSampleCount++;
      lockWeightSumGrams += liveWeightGrams;
    }
  } else {
    lastLockCandidateGrams = liveWeightGrams;
    lockMatchCount = 1;
    lockWeightSampleCount = 1;
    lockWeightSumGrams = liveWeightGrams;
  }

  if (lockMatchCount >= LOCK_MATCH_SAMPLES) {
    const float averageStableWeightGrams = lockWeightSumGrams / lockWeightSampleCount;
    lockedWeightGrams = round(averageStableWeightGrams);
    currentWeightGrams = lockedWeightGrams;
    weightLocked = true;
    Serial.print("Weight locked(g): ");
    Serial.print(lockedWeightGrams, 0);
    Serial.print(" avg=");
    Serial.println(averageStableWeightGrams, 2);
  }
}

void setupScale() {
  lcd.clear();
  lcd.print("Starting scale...");

  EEPROM.begin(512);
  EEPROM.get(calVal_eepromAddress, calibration_factor);
  if (!isValidCalibrationFactor(calibration_factor)) {
    calibration_factor = DEFAULT_CALIBRATION_FACTOR;
    persistCalibrationFactor(calibration_factor);
  } else if (fabs(calibration_factor - OLD_DEFAULT_CALIBRATION_FACTOR) < 0.0001) {
    calibration_factor = DEFAULT_CALIBRATION_FACTOR;
    persistCalibrationFactor(calibration_factor);
  } else if (fabs(calibration_factor - PREVIOUS_CALIBRATION_FACTOR) < 0.0001) {
    calibration_factor = DEFAULT_CALIBRATION_FACTOR;
    persistCalibrationFactor(calibration_factor);
  }

  Serial.println("Starting HX711...");
  Serial.print("DOUT GPIO: ");
  Serial.println(HX_DOUT);
  Serial.print("SCK GPIO: ");
  Serial.println(HX_SCK);
  Serial.print("Calibration factor: ");
  Serial.println(calibration_factor, 6);

  LoadCell.begin();
  unsigned long stabilizingtime = 2000;
  boolean tare = true;
  LoadCell.start(stabilizingtime, tare);

  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
    hx711Ready = false;
    Serial.println("HX711 timeout - check wiring");
    lcd.clear();
    lcd.print("HX711 timeout");
    lcd.setCursor(0, 1);
    lcd.print("Check wiring");
    return;
  }

  LoadCell.setCalFactor(calibration_factor);
  hx711Ready = true;
  currentWeightGrams = 0.0;
  lastHx711UpdateMs = millis();
  Serial.println("Scale ready");
  printScaleHelp();
}

void setupRtc() {
  if (!rtc.begin()) {
    rtcReady = false;
    Serial.println("RTC not found");
    return;
  }

  rtcReady = true;
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(__DATE__, __TIME__));
  }
}

void setupWifi() {
  lcd.clear();
  lcd.print("Connecting WiFi...");
  lcd.setCursor(0, 1);
  lcd.print(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    lcd.setCursor(tries % LCD_COLS, 2);
    lcd.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi OK");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    lcd.clear();
    lcd.print("WiFi connected");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
  } else {
    Serial.println("WiFi FAILED");
    WiFi.disconnect(false, false);
    lcd.clear();
    lcd.print("WiFi failed");
    lcd.setCursor(0, 1);
    lcd.print("Check SSID/pass");
    lcd.setCursor(0, 2);
    lcd.print("Offline sync queue");
  }
  delay(WIFI_RESULT_DISPLAY_MS);
}

String macToString(const uint8_t* mac) {
  char buffer[18];
  snprintf(
    buffer,
    sizeof(buffer),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
  return String(buffer);
}

bool addEspNowBroadcastPeer() {
  if (esp_now_is_peer_exist(broadcastPeer)) {
    return true;
  }

  uint8_t channel = WiFi.channel();
  if (channel == 0) {
    channel = 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastPeer, 6);
  peerInfo.channel = channel;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add ESP-NOW broadcast peer");
    return false;
  }

  return true;
}

void sendCameraCommand(const char* command) {
  if (!espNowReady) {
    return;
  }

  ScaleCommandPacket packet = {};
  packet.packetType = PACKET_TYPE_SCALE_COMMAND;
  strlcpy(packet.command, command, sizeof(packet.command));
  packet.sequence = ++cameraCommandSequence;
  packet.uptime_ms = millis();

  esp_err_t result = esp_now_send(
    broadcastPeer,
    reinterpret_cast<uint8_t*>(&packet),
    sizeof(packet)
  );
  if (result != ESP_OK) {
    Serial.printf("Camera command %s failed: %d\n", command, result);
  } else {
    Serial.print("Camera command sent: ");
    Serial.println(command);
  }
}

void onEspNowReceived(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len) {
  if (len != sizeof(DetectionPacket)) {
    return;
  }

  DetectionPacket packet = {};
  memcpy(&packet, data, sizeof(packet));
  packet.label[sizeof(packet.label) - 1] = '\0';
  if (packet.packetType != PACKET_TYPE_DETECTION_RESULT) {
    return;
  }

  const bool confident =
    packet.confidence >= FRUIT_DETECTION_CONFIDENCE &&
    strcmp(packet.label, "Unknown") != 0;
  strlcpy(
    currentFruitType,
    confident ? packet.label : "Unknown",
    sizeof(currentFruitType)
  );
  cameraResultReceived = true;
  cameraDetectionRequested = false;

  Serial.print("Camera result from ");
  Serial.print(macToString(recvInfo->src_addr));
  Serial.print(": ");
  Serial.print(currentFruitType);
  Serial.print(" confidence=");
  Serial.println(packet.confidence, 4);
}

bool setupEspNow() {
  uint8_t channel = WiFi.channel();
  if (channel == 0) {
    channel = 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return false;
  }

  esp_now_register_recv_cb(onEspNowReceived);
  if (!addEspNowBroadcastPeer()) {
    return false;
  }

  espNowReady = true;
  Serial.print("Scale MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("ESP-NOW channel: ");
  Serial.println(channel);
  return true;
}

void requestFruitDetection() {
  if (cameraResultReceived || cameraScanAttemptedForCurrentObject) {
    return;
  }

  cameraScanAttemptedForCurrentObject = true;
  cameraDetectionRequested = true;
  cameraDetectionStartedMs = millis();
  strlcpy(currentFruitType, "Processing", sizeof(currentFruitType));
  sendCameraCommand("START");
}

void stopFruitDetection(bool clearFruit) {
  if (cameraDetectionRequested || cameraResultReceived) {
    sendCameraCommand("STOP");
  }
  cameraDetectionRequested = false;
  if (clearFruit) {
    cameraResultReceived = false;
    cameraScanAttemptedForCurrentObject = false;
  }
  cameraDetectionStartedMs = 0;
  if (clearFruit) {
    strlcpy(currentFruitType, "Unknown", sizeof(currentFruitType));
  }
}

void maintainFruitDetection() {
  if (!cameraDetectionRequested || cameraResultReceived) {
    return;
  }

  if (millis() - cameraDetectionStartedMs >= CAMERA_DETECTION_TIMEOUT_MS) {
    strlcpy(currentFruitType, "Unknown", sizeof(currentFruitType));
    cameraDetectionRequested = false;
    cameraResultReceived = true;
    sendCameraCommand("STOP");
    Serial.println("Camera detection timeout - fruit Unknown");
    return;
  }
}

void syncRtcFromNtp() {
  if (!rtcReady || WiFi.status() != WL_CONNECTED) return;

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("NTP sync failed");
    return;
  }

  rtc.adjust(DateTime(
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec));
  Serial.println("RTC synced from NTP");
}

String currentTimestamp() {
  if (!rtcReady) return "";

  DateTime now = rtc.now();
  char timestamp[25];
  snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d",
           now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  return String(timestamp);
}

void copyCurrentDateTime(char* timestampDestination, size_t timestampSize,
                         char* dateDestination, size_t dateSize,
                         char* timeDestination, size_t timeSize) {
  if (timestampSize > 0) timestampDestination[0] = '\0';
  if (dateSize > 0) dateDestination[0] = '\0';
  if (timeSize > 0) timeDestination[0] = '\0';

  if (!rtcReady) {
    if (timestampSize > 0) snprintf(timestampDestination, timestampSize, "unknown");
    if (dateSize > 0) snprintf(dateDestination, dateSize, "unknown");
    if (timeSize > 0) snprintf(timeDestination, timeSize, "unknown");
    return;
  }

  DateTime now = rtc.now();
  if (timestampSize > 0) {
    snprintf(timestampDestination, timestampSize, "%04d-%02d-%02dT%02d:%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  }
  if (dateSize > 0) {
    snprintf(dateDestination, dateSize, "%04d-%02d-%02d",
             now.year(), now.month(), now.day());
  }
  if (timeSize > 0) {
    snprintf(timeDestination, timeSize, "%02d:%02d:%02d",
             now.hour(), now.minute(), now.second());
  }
}

String saleRecordJson(const SaleRecord& sale) {
  return saleRecordJson(sale, "  ");
}

String saleRecordJson(const SaleRecord& sale, const String& indent) {
  String body = "{\n";
  body += indent;
  body += "\"id\": ";
  body += String(sale.id);
  body += ",\n";
  body += indent;
  body += "\"weightGrams\": ";
  body += String(sale.weightGrams, 2);
  body += ",\n";
  body += indent;
  body += "\"weight\": ";
  body += String(sale.weightGrams, 2);
  body += ",\n";
  body += indent;
  body += "\"weightKg\": ";
  body += String(sale.weightGrams / 1000.0, 3);
  body += ",\n";
  body += indent;
  body += "\"price\": ";
  body += String(sale.price, 2);
  body += ",\n";
  body += indent;
  body += "\"priceCentavos\": ";
  body += String((int)round(sale.price * 100.0));
  body += ",\n";
  body += indent;
  body += "\"pricePerKg\": ";
  body += String(sale.pricePerKg, 2);
  body += ",\n";
  body += indent;
  body += "\"pricePerKgCentavos\": ";
  body += String((int)round(sale.pricePerKg * 100.0));
  body += ",\n";
  body += indent;
  body += "\"fruitType\": \"";
  body += jsonEscape(sale.fruitType);
  body += "\",\n";
  body += indent;
  body += "\"fruit\": \"";
  body += jsonEscape(sale.fruitType);
  body += "\",\n";
  body += indent;
  body += "\"fruit_type\": \"";
  body += jsonEscape(sale.fruitType);
  body += "\",\n";
  body += indent;
  body += "\"timestamp\": \"";
  body += jsonEscape(sale.timestamp);
  body += "\",\n";
  body += indent;
  body += "\"soldAt\": \"";
  body += jsonEscape(sale.timestamp);
  body += "\",\n";
  body += indent;
  body += "\"date\": \"";
  body += jsonEscape(sale.date);
  body += "\",\n";
  body += indent;
  body += "\"time\": \"";
  body += jsonEscape(sale.time);
  body += "\",\n";
  body += indent;
  body += "\"source\": \"";
  body += jsonEscape(sale.source);
  body += "\",\n";
  body += indent;
  body += "\"sourceDeviceId\": \"";
  body += jsonEscape(firebaseScaleDeviceId);
  body += "\",\n";
  body += indent;
  body += "\"status\": \"sold\",\n";
  body += indent;
  body += "\"imported\": false,\n";
  body += indent;
  body += "\"createdAtMs\": ";
  body += String(sale.createdAtMs);
  body += "\n";
  body += "}";
  return body;
}

SaleRecord recordSale(const char* source) {
  SaleRecord sale = {};
  sale.id = nextSaleId++;
  sale.weightGrams = currentWeightGrams;
  strlcpy(sale.fruitType, currentFruitType, sizeof(sale.fruitType));
  sale.pricePerKg = pricePerKgForFruit(sale.fruitType);
  sale.price = calculatePriceForFruit(sale.fruitType, currentWeightGrams);
  sale.createdAtMs = millis();
  strlcpy(sale.source, source, sizeof(sale.source));
  copyCurrentDateTime(sale.timestamp, sizeof(sale.timestamp),
                      sale.date, sizeof(sale.date),
                      sale.time, sizeof(sale.time));
  const String firebaseKey =
    firebaseSafeKey(String(sale.id) + "_" + sale.timestamp + "_" + String(sale.createdAtMs));
  firebaseKey.toCharArray(sale.firebaseKey, sizeof(sale.firebaseKey));

  latestSaleId = sale.id;
  if (saleHistoryCount >= SALE_HISTORY_SIZE) {
    for (size_t i = 1; i < SALE_HISTORY_SIZE; i++) {
      saleHistory[i - 1] = saleHistory[i];
    }
    saleHistoryCount = SALE_HISTORY_SIZE - 1;
  }
  saleHistory[saleHistoryCount] = sale;
  saleHistoryCount++;

  return sale;
}

void updateDisplay() {
  float billableWeightGrams = currentWeightGrams;
  if (billableWeightGrams < 0) billableWeightGrams = 0;
  float price = calculatePrice(billableWeightGrams);
  char statusCode = 'Z';
  if (weightLocked) {
    statusCode = 'L';
  } else if (objectPresent) {
    statusCode = 'W';
  }

  char line[21];
  char weightText[9];
  formatDisplayWeight(billableWeightGrams, weightText, sizeof(weightText));

  snprintf(line, sizeof(line), "Fruit:%-13.13s", currentFruitType);
  printPadded(0, 0, line);

  if (hasFruitPrice(currentFruitType)) {
    snprintf(line, sizeof(line), "Wt:%-7s P:%6.2f", weightText, price);
  } else {
    snprintf(line, sizeof(line), "Wt:%-7s P: --", weightText);
  }
  printPadded(0, 1, line);

  if (rtcReady) {
    DateTime now = rtc.now();
    snprintf(line, sizeof(line), "Stat:%c %02d:%02d", statusCode, now.hour(), now.minute());
    printPadded(0, 2, line);
    snprintf(line, sizeof(line), "%02d/%02d/%04d",
             now.month(), now.day(), now.year());
  } else {
    snprintf(line, sizeof(line), "Stat:%c No RTC", statusCode);
    printPadded(0, 2, line);
    snprintf(line, sizeof(line), "RTC not detected");
  }
  printPadded(0, 3, line);
}

String firebaseSafeKey(const char* value) {
  String key = "";
  for (size_t i = 0; value[i] != '\0'; i++) {
    const char c = value[i];
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_') {
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

String firebaseSaleKey(const SaleRecord& sale) {
  if (strlen(sale.firebaseKey) > 0) {
    return firebaseSafeKey(sale.firebaseKey);
  }
  String key = String(sale.id);
  key += "_";
  key += sale.timestamp;
  key += "_";
  key += String(sale.createdAtMs);
  return firebaseSafeKey(key);
}

String firebaseBaseUrl() {
  String base = firebaseDatabaseUrl;
  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  return base;
}

String firebaseUrlForPath(const String& path) {
  String url = firebaseBaseUrl();
  if (!path.startsWith("/")) {
    url += "/";
  }
  url += path;
  url += ".json";
  if (strlen(firebaseAuthToken) > 0) {
    url += "?auth=";
    url += firebaseAuthToken;
  }
  return url;
}

String firebaseRequestUrl(const SaleRecord& sale) {
  String path = "scaleSales/";
  path += firebaseSafeKey(firebaseScaleDeviceId);
  path += "/";
  path += firebaseSaleKey(sale);
  return firebaseUrlForPath(path);
}

int jsonValueStart(const String& json, const char* key, int searchFrom = 0) {
  String pattern = "\"";
  pattern += key;
  pattern += "\"";
  int keyIndex = json.indexOf(pattern, searchFrom);
  if (keyIndex < 0) {
    return -1;
  }

  int colonIndex = json.indexOf(':', keyIndex + pattern.length());
  if (colonIndex < 0) {
    return -1;
  }

  int valueStart = colonIndex + 1;
  while (valueStart < json.length()) {
    char c = json.charAt(valueStart);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    valueStart++;
  }
  return valueStart;
}

bool extractJsonString(const String& json, const char* key, String& value, int searchFrom = 0) {
  int start = jsonValueStart(json, key, searchFrom);
  if (start < 0 || start >= json.length() || json.charAt(start) != '"') {
    return false;
  }

  String parsed = "";
  bool escaped = false;
  for (int i = start + 1; i < json.length(); i++) {
    char c = json.charAt(i);
    if (escaped) {
      parsed += c;
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      value = parsed;
      return true;
    } else {
      parsed += c;
    }
  }
  return false;
}

bool isJsonNumberChar(char c) {
  return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
}

bool extractJsonNumber(const String& json, const char* key, String& value, int searchFrom = 0) {
  int start = jsonValueStart(json, key, searchFrom);
  if (start < 0 || start >= json.length()) {
    return false;
  }

  int end = start;
  while (end < json.length() && isJsonNumberChar(json.charAt(end))) {
    end++;
  }

  if (end == start) {
    return false;
  }

  value = json.substring(start, end);
  return true;
}

bool firebaseReadBackoffActive() {
  return firebaseReadBackoffUntilMs != 0 &&
         static_cast<long>(firebaseReadBackoffUntilMs - millis()) > 0;
}

bool firebaseUploadBackoffActive() {
  return firebaseUploadBackoffUntilMs != 0 &&
         static_cast<long>(firebaseUploadBackoffUntilMs - millis()) > 0;
}

void noteFirebaseReadFailure(int statusCode = 0) {
  firebaseReadBackoffUntilMs = millis() + FIREBASE_READ_FAILURE_BACKOFF_MS;
  Serial.print("Firebase ");
  Serial.print("read");
  Serial.print(" paused ");
  Serial.print(FIREBASE_READ_FAILURE_BACKOFF_MS / 1000);
  Serial.print("s");
  if (statusCode != 0) {
    Serial.print(" after HTTP ");
    Serial.print(statusCode);
  }
  Serial.println();
}

void noteFirebaseUploadFailure(int statusCode = 0) {
  firebaseUploadBackoffUntilMs = millis() + FIREBASE_UPLOAD_FAILURE_BACKOFF_MS;
  Serial.print("Firebase upload paused ");
  Serial.print(FIREBASE_UPLOAD_FAILURE_BACKOFF_MS / 1000);
  Serial.print("s");
  if (statusCode != 0) {
    Serial.print(" after HTTP ");
    Serial.print(statusCode);
  }
  Serial.println();
}

bool getFirebaseJson(const String& path, String& payload) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(FIREBASE_HTTP_TIMEOUT_MS);
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
    noteFirebaseReadFailure(statusCode);
    return false;
  }

  return true;
}

bool applyPriceObject(const String& json, int searchFrom = 0) {
  String fruit;
  if (!extractJsonString(json, "fruit", fruit, searchFrom)) {
    return false;
  }

  String priceCentavosText;
  float price = 0.0f;
  if (extractJsonNumber(json, "priceCentavos", priceCentavosText, searchFrom)) {
    price = priceCentavosText.toInt() / 100.0f;
  } else {
    String priceText;
    if (extractJsonNumber(json, "price", priceText, searchFrom)) {
      price = priceText.toFloat();
    }
  }

  if (price <= 0.0f) {
    Serial.print("Ignored invalid price update for ");
    Serial.println(fruit);
    return false;
  }

  char fruitBuffer[32];
  fruit.toCharArray(fruitBuffer, sizeof(fruitBuffer));
  return setFruitPrice(fruitBuffer, price);
}

bool applyPriceTablePayload(const String& payload) {
  if (payload.length() == 0 || payload == "null") {
    return false;
  }

  bool updatedAny = false;
  int searchFrom = 0;
  while (true) {
    int fruitIndex = payload.indexOf("\"fruit\"", searchFrom);
    if (fruitIndex < 0) {
      break;
    }
    if (applyPriceObject(payload, fruitIndex)) {
      updatedAny = true;
    }
    searchFrom = fruitIndex + 7;
  }
  return updatedAny;
}

bool fetchPriceTable() {
  String path = "scalePriceUpdates/";
  path += firebaseSafeKey(firebaseScaleDeviceId);
  path += "/fruits";

  String payload;
  if (!getFirebaseJson(path, payload)) {
    return false;
  }

  const bool updated = applyPriceTablePayload(payload);
  if (updated) {
    Serial.println("Scale price table synced from Firebase");
  }
  return true;
}

bool fetchLatestPriceUpdate() {
  String path = "scalePriceUpdates/";
  path += firebaseSafeKey(firebaseScaleDeviceId);
  path += "/latest";

  String payload;
  if (!getFirebaseJson(path, payload) || payload.length() == 0 || payload == "null") {
    return false;
  }

  String version;
  if (!extractJsonNumber(payload, "version", version)) {
    version = payload;
  }

  if (version == lastPriceUpdateVersion) {
    return true;
  }

  const bool appliedLatest = applyPriceObject(payload);
  lastPriceUpdateVersion = version;
  fetchPriceTable();
  return appliedLatest;
}

void maintainPriceUpdates() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (firebaseReadBackoffActive()) {
    return;
  }

  const unsigned long now = millis();
  if (lastPriceTableRefreshMs == 0 ||
      now - lastPriceTableRefreshMs >= PRICE_TABLE_REFRESH_MS) {
    lastPriceTableRefreshMs = now;
    fetchPriceTable();
    if (firebaseReadBackoffActive()) {
      return;
    }
  }

  if (now - lastPriceUpdatePollMs >= PRICE_UPDATE_POLL_MS) {
    lastPriceUpdatePollMs = now;
    fetchLatestPriceUpdate();
  }
}

bool uploadSaleToFirebase(const SaleRecord& sale) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase upload skipped: WiFi disconnected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(FIREBASE_HTTP_TIMEOUT_MS);
  const String url = firebaseRequestUrl(sale);
  if (!http.begin(client, url)) {
    Serial.println("Firebase upload failed: could not start HTTPS request");
    noteFirebaseUploadFailure();
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  const int statusCode = http.PUT(saleRecordJson(sale));
  const String response = http.getString();
  http.end();

  Serial.print("Firebase upload HTTP ");
  Serial.print(statusCode);
  if (statusCode >= 200 && statusCode < 300) {
    Serial.print(": ");
    Serial.println(response);
  } else {
    Serial.println();
    noteFirebaseUploadFailure(statusCode);
  }

  return statusCode >= 200 && statusCode < 300;
}

bool uploadPendingSales() {
  if (saleHistoryCount == 0) {
    return true;
  }
  if (firebaseUploadBackoffActive()) {
    return false;
  }

  if (saleHistory[0].id != 0 && !uploadSaleToFirebase(saleHistory[0])) {
    Serial.print("Firebase pending sales remaining: ");
    Serial.println(saleHistoryCount);
    return false;
  }

  for (size_t i = 1; i < saleHistoryCount; i++) {
    saleHistory[i - 1] = saleHistory[i];
  }
  saleHistoryCount--;
  saleHistory[saleHistoryCount] = SaleRecord{};

  Serial.print("Firebase pending sales uploaded: 1, remaining: ");
  Serial.println(saleHistoryCount);

  return saleHistoryCount == 0;
}

bool maintainPendingSaleUpload() {
  if (WiFi.status() != WL_CONNECTED ||
      saleHistoryCount == 0 ||
      firebaseUploadBackoffActive() ||
      millis() - lastFirebaseRetryMs < FIREBASE_RETRY_MS) {
    return saleHistoryCount == 0;
  }

  lastFirebaseRetryMs = millis();
  return uploadPendingSales();
}

void setCurrentFruitType(const String& fruit) {
  String cleanFruit = fruit;
  cleanFruit.trim();
  if (cleanFruit.length() == 0) {
    Serial.println("Fruit name is empty.");
    return;
  }

  cleanFruit.toCharArray(currentFruitType, sizeof(currentFruitType));
  cameraResultReceived = true;
  cameraDetectionRequested = false;
  cameraScanAttemptedForCurrentObject = true;
  sendCameraCommand("STOP");
  Serial.print("Current fruit set to: ");
  Serial.println(currentFruitType);
  showMessage("Fruit selected", currentFruitType);
}

void handleButtons() {
  unsigned long nowMs = millis();
  bool successPressed = updateButton(successButton, nowMs);
  bool cancelPressed = updateButton(cancelButton, nowMs);

  if (successPressed && nowMs - lastSuccessActionMs >= BUTTON_COOLDOWN_MS) {
    lastSuccessActionMs = nowMs;
    confirmSale("SALE OK", "scale");
  }

  if (cancelPressed && nowMs - lastCancelActionMs >= BUTTON_COOLDOWN_MS) {
    lastCancelActionMs = nowMs;
    cancelSale("Cancel button tare");
  }
}

void processSerialCommand() {
  if (Serial.available() == 0) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.length() == 0) return;

  if (command == "t" || command == "T") {
    tareScale("Serial tare");
    return;
  }

  if (command == "+" || command == "a" || command == "A") {
    saveCalibrationFactor(calibration_factor + CALIBRATION_STEP);
    return;
  }

  if (command == "-" || command == "z" || command == "Z") {
    saveCalibrationFactor(calibration_factor - CALIBRATION_STEP);
    return;
  }

  if (command == "r" || command == "R") {
    saveCalibrationFactor(DEFAULT_CALIBRATION_FACTOR);
    return;
  }

  if (command[0] == 'c' || command[0] == 'C') {
    float knownWeightGrams = command.substring(1).toFloat();
    if (!hx711Ready || knownWeightGrams <= 0) {
      Serial.println("Use: c 500");
      return;
    }
    LoadCell.refreshDataSet();
    saveCalibrationFactor(LoadCell.getNewCalibration(knownWeightGrams));
    resetWeightState();
    currentWeightGrams = readWeightGrams();
    return;
  }

  if (command.startsWith("fruit ") || command.startsWith("Fruit ") ||
      command.startsWith("FRUIT ")) {
    setCurrentFruitType(command.substring(6));
    return;
  }

  if (command.startsWith("f ") || command.startsWith("F ")) {
    setCurrentFruitType(command.substring(2));
    return;
  }

  printScaleHelp();
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);

  pinMode(BTN_SUCCESS, INPUT_PULLUP);
  pinMode(BTN_CANCEL, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  delay(20);
  beginButton(successButton);
  beginButton(cancelButton);

  lastSuccessActionMs = millis() - BUTTON_COOLDOWN_MS;
  lastCancelActionMs = millis() - BUTTON_COOLDOWN_MS;

  Wire.begin(I2C_SDA, I2C_SCL);

  int lcdStatus = lcd.begin(LCD_COLS, LCD_ROWS);
  if (lcdStatus) {
    hd44780::fatalError(lcdStatus);
  }
  lcd.backlight();
  lcd.print("Initializing...");

  setupRtc();
  setupWifi();
  setupEspNow();
  syncRtcFromNtp();
  setupScale();
  maintainPriceUpdates();

  showMessage(hx711Ready ? "Ready!" : "Scale not ready");
}

void loop() {
  if (hx711Ready && LoadCell.update()) {
    lastHx711UpdateMs = millis();
    newScaleData = true;
  }

  if (hx711Ready) {
    updateLockedWeight();
  }

  handleButtons();
  processSerialCommand();
  maintainFruitDetection();
  maintainPriceUpdates();
  maintainPendingSaleUpload();

  if (millis() - lastSerialDiagnosticMs >= SERIAL_DIAGNOSTIC_INTERVAL_MS) {
    printScaleDiagnostics();
    if (hx711Ready && millis() - lastHx711UpdateMs > 2000) {
      Serial.println("HX711 not updating - check DOUT/SCK wiring and power.");
    }
    lastSerialDiagnosticMs = millis();
  }

  if (millis() - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    if (millis() >= messageUntilMs) {
      updateDisplay();
    }
    lastDisplayMs = millis();
  }
}
