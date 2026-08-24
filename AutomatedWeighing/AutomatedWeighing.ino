#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>
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

#include "protocol.h"
#include "config.h"
#include "json_utils.h"
#include "prices.h"

// Aliases for the packet-type names this sketch already used.
constexpr uint8_t PACKET_TYPE_SCALE_COMMAND = kPacketTypeScaleCommand;
constexpr uint8_t PACKET_TYPE_DETECTION_RESULT = kPacketTypeDetectionResult;
constexpr uint8_t PACKET_TYPE_SALE_SYNC = kPacketTypeSaleSync;
constexpr uint8_t PACKET_TYPE_PRICE_UPDATE = kPacketTypePriceUpdate;
constexpr uint8_t PACKET_TYPE_SALE_ACK = kPacketTypeSaleAck;

uint8_t broadcastPeer[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint8_t CALIBRATION_SAMPLE_READS = 8;
constexpr uint8_t CALIBRATION_REQUIRED_SAMPLES = 5;
constexpr uint8_t TARE_SAMPLE_READS = 32;
constexpr unsigned long CALIBRATION_SAMPLE_TIMEOUT_MS = 3000;
constexpr uint32_t CALIBRATION_EEPROM_MAGIC = 0x4E415531UL;  // "NAU1"

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

hd44780_I2Cexp lcd(0x27);
NAU7802 loadCell;
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
bool calibrationLearningActive = false;
unsigned int calibrationLearningSampleCount = 0;
float calibrationLearningSavedFactor = 0.0;
bool calibrationLearningSavedReady = false;
double calibrationLearningWeightRawSum = 0.0;
double calibrationLearningWeightSquaredSum = 0.0;
float calibrationLearningCandidateFactor = 0.0;
float calibrationLearningKnownWeightsGrams[CALIBRATION_REQUIRED_SAMPLES] = {};
int32_t calibrationLearningRawDeltas[CALIBRATION_REQUIRED_SAMPLES] = {};
int32_t calibrationLearningZeroOffset = 0;
float currentWeightGrams = 0.0;
unsigned long lastDisplayMs = 0;
unsigned long lastSerialDiagnosticMs = 0;
unsigned long lastNau7802UpdateMs = 0;
unsigned long lastSuccessActionMs = 0;
unsigned long lastCancelActionMs = 0;
unsigned long messageUntilMs = 0;
float filteredWeightGrams = 0.0;
float lastSensorWeightGrams = 0.0;
float lastProcessedWeightGrams = 0.0;
float lastReturnedWeightGrams = 0.0;
float lastFilterAlpha = 0.0;
int32_t lastNau7802RawReading = 0;
bool nau7802Ready = false;
bool calibrationReady = false;
bool rtcReady = false;
bool objectPresent = false;
bool newScaleData = false;
uint8_t objectDetectCount = 0;
uint8_t objectRemoveCount = 0;
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
unsigned long objectRedetectCooldownUntilMs = 0;
String lastPriceUpdateVersion = "";
unsigned long objectPresentStartedMs = 0;
unsigned long cameraDetectionStartedMs = 0;
uint8_t cameraDetectionRetries = 0;
uint32_t cameraCommandSequence = 0;
uint32_t saleSyncSequence = 0;
bool espNowReady = false;
bool cameraDetectionRequested = false;
bool cameraResultReceived = false;
bool cameraScanAttemptedForCurrentObject = false;
bool saleConfirmedForCurrentObject = false;
bool cancelledObjectActive = false;

SaleRecord recordSale(const char* source);
String saleRecordJson(const SaleRecord& sale);
String saleRecordJson(const SaleRecord& sale, const String& indent);
bool uploadPendingSales();
bool maintainPendingSaleUpload();
bool sendSaleToFirebaseWorker(const SaleRecord& sale);
bool setupEspNow();
void requestFruitDetection();
void stopFruitDetection(bool clearFruit);
void maintainFruitDetection();
void maintainPriceUpdates();
void showMessage(const char* line1, const char* line2 = "", unsigned long nowMs = millis());
bool fetchPriceTable();
bool fetchLatestPriceUpdate();

float calculatePrice(float weightGrams) {
  if (weightGrams < 0) weightGrams = 0;
  return (weightGrams / 1000.0) * pricePerKgForFruit(currentFruitType);
}

float calculatePriceForFruit(const char* fruitType, float weightGrams) {
  if (weightGrams < 0) weightGrams = 0;
  return (weightGrams / 1000.0) * pricePerKgForFruit(fruitType);
}

const char* scaleStatusText() {
  if (calibrationLearningActive) return "calibrating";
  if (!calibrationReady) return "uncalibrated";
  if (cancelledObjectActive) return "cancelled";
  if (objectPresent) return "weighing";
  if (objectDetectCount > 0) return "detecting";
  return "zero";
}

bool isValidCalibrationFactor(float value) {
  const float magnitude = fabs(value);
  return !isnan(value) && !isinf(value) &&
         magnitude >= 0.01f && magnitude <= 10000000.0f;
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
  snprintf(buffer, bufferSize, "%.3fkg", weightKg);
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
  objectPresent = false;
  cameraScanAttemptedForCurrentObject = false;
  saleConfirmedForCurrentObject = false;
  cancelledObjectActive = false;
  currentWeightGrams = 0.0;
  filteredWeightGrams = 0.0;
  lastSensorWeightGrams = 0.0;
  lastProcessedWeightGrams = 0.0;
  lastReturnedWeightGrams = 0.0;
  lastFilterAlpha = 0.0;
  objectDetectCount = 0;
  objectRemoveCount = 0;
  objectPresentStartedMs = 0;
}

bool objectRedetectCooldownActive() {
  return objectRedetectCooldownUntilMs != 0 &&
         static_cast<long>(objectRedetectCooldownUntilMs - millis()) > 0;
}

void startObjectRedetectCooldown() {
  objectRedetectCooldownUntilMs = millis() + OBJECT_REDETECT_COOLDOWN_MS;
}

void persistCalibrationFactor(float value, bool ready) {
  EEPROM.put(calVal_eepromAddress, value);
  const uint32_t marker = ready ? CALIBRATION_EEPROM_MAGIC : 0;
  EEPROM.put(calVal_eepromAddress + sizeof(value), marker);
  EEPROM.commit();
}

bool readNau7802AverageRaw(
  uint8_t samplesToRead,
  unsigned long timeoutMs,
  int32_t &averageRaw,
  int32_t &minimumRaw,
  int32_t &maximumRaw
) {
  if (!nau7802Ready || samplesToRead == 0) {
    return false;
  }

  int64_t total = 0;
  uint8_t samplesRead = 0;
  const unsigned long startedMs = millis();

  while (samplesRead < samplesToRead && millis() - startedMs < timeoutMs) {
    if (!loadCell.available()) {
      delay(1);
      continue;
    }

    const int32_t raw = loadCell.getReading();
    lastNau7802RawReading = raw;
    lastNau7802UpdateMs = millis();

    if (samplesRead == 0) {
      minimumRaw = raw;
      maximumRaw = raw;
    } else {
      if (raw < minimumRaw) minimumRaw = raw;
      if (raw > maximumRaw) maximumRaw = raw;
    }

    total += raw;
    samplesRead++;
  }

  if (samplesRead != samplesToRead) {
    return false;
  }

  averageRaw = static_cast<int32_t>(total / samplesRead);
  return true;
}

bool captureNau7802Reading() {
  if (!nau7802Ready || !loadCell.available()) {
    return false;
  }

  lastNau7802RawReading = loadCell.getReading();
  lastNau7802UpdateMs = millis();
  if (calibrationReady) {
    lastSensorWeightGrams =
      static_cast<float>(lastNau7802RawReading - loadCell.getZeroOffset()) /
      calibration_factor;
  } else {
    lastSensorWeightGrams = 0.0f;
  }

  if (isnan(lastSensorWeightGrams) || isinf(lastSensorWeightGrams)) {
    Serial.println("Invalid NAU7802 reading");
    return false;
  }

  return true;
}

void saveCalibrationFactor(float value, bool ready = true) {
  if (!isValidCalibrationFactor(value)) {
    Serial.println("Invalid calibration factor, not saved.");
    return;
  }

  calibration_factor = value;
  calibrationReady = ready;
  loadCell.setCalibrationFactor(calibration_factor);
  persistCalibrationFactor(calibration_factor, calibrationReady);
  Serial.print("Calibration factor saved: ");
  Serial.print(calibration_factor, 6);
  Serial.print(" ready=");
  Serial.println(calibrationReady ? "yes" : "no");
}

void clearCalibrationLearningSession() {
  calibrationLearningActive = false;
  calibrationLearningSampleCount = 0;
  calibrationLearningSavedFactor = 0.0;
  calibrationLearningSavedReady = false;
  calibrationLearningWeightRawSum = 0.0;
  calibrationLearningWeightSquaredSum = 0.0;
  calibrationLearningCandidateFactor = 0.0;
  calibrationLearningZeroOffset = 0;
  for (uint8_t i = 0; i < CALIBRATION_REQUIRED_SAMPLES; i++) {
    calibrationLearningKnownWeightsGrams[i] = 0.0f;
    calibrationLearningRawDeltas[i] = 0;
  }
}

void beginCalibrationLearning() {
  calibrationLearningActive = true;
  calibrationLearningSampleCount = 0;
  calibrationLearningSavedFactor = calibration_factor;
  calibrationLearningSavedReady = calibrationReady;
  calibrationLearningWeightRawSum = 0.0;
  calibrationLearningWeightSquaredSum = 0.0;
  calibrationLearningCandidateFactor = calibration_factor;
  calibrationLearningZeroOffset = loadCell.getZeroOffset();
  for (uint8_t i = 0; i < CALIBRATION_REQUIRED_SAMPLES; i++) {
    calibrationLearningKnownWeightsGrams[i] = 0.0f;
    calibrationLearningRawDeltas[i] = 0;
  }

  resetWeightState();

  Serial.println("Five-point calibration started.");
  Serial.println("Tare only once before cal start; do not tare between samples.");
  Serial.println("Recommended: cal add 20, 50, 100, 200, and 500.");
  Serial.println("Replace the weight and wait for it to settle before each command.");
  Serial.println("After all five samples, use: cal save");
  Serial.println("Use: cal cancel to return to the last saved factor.");
  showMessage("5-point cal", "Add sample 1/5");
}

bool hasCalibrationLearningSamples() {
  return calibrationLearningSampleCount > 0 &&
         calibrationLearningWeightSquaredSum > 0.0 &&
         isValidCalibrationFactor(calibrationLearningCandidateFactor);
}

void printCalibrationLearningStatus() {
  Serial.print("Calibration learning active=");
  Serial.print(calibrationLearningActive ? "yes" : "no");
  Serial.print(" samples=");
  Serial.print(calibrationLearningSampleCount);
  Serial.print("/");
  Serial.print(CALIBRATION_REQUIRED_SAMPLES);
  Serial.print(" current=");
  Serial.print(calibration_factor, 6);
  Serial.print(" ready=");
  Serial.print(calibrationReady ? "yes" : "no");
  Serial.print(" candidate=");
  if (hasCalibrationLearningSamples()) {
    Serial.print(calibrationLearningCandidateFactor, 6);
  } else {
    Serial.print("none");
  }
  Serial.print(" saved=");
  Serial.print(calibrationLearningSavedFactor, 6);
  Serial.print(" points=");
  for (uint8_t i = 0; i < calibrationLearningSampleCount; i++) {
    if (i > 0) Serial.print(",");
    Serial.print(calibrationLearningKnownWeightsGrams[i], 0);
    Serial.print("g");
  }
  Serial.println();
}

bool addCalibrationLearningSample(float knownWeightGrams) {
  if (!nau7802Ready || knownWeightGrams <= 0.0f) {
    Serial.println("Use: cal add 500");
    return false;
  }

  if (!calibrationLearningActive) {
    beginCalibrationLearning();
  }

  if (calibrationLearningSampleCount >= CALIBRATION_REQUIRED_SAMPLES) {
    Serial.println("Five calibration samples already accepted. Use cal save or cal cancel.");
    showMessage("Cal points full", "Send cal save");
    return false;
  }

  if (loadCell.getZeroOffset() != calibrationLearningZeroOffset) {
    Serial.println("Zero offset changed during calibration. Use cal cancel, tare empty, then cal start.");
    showMessage("Cal zero changed", "Restart cal");
    return false;
  }

  for (uint8_t i = 0; i < calibrationLearningSampleCount; i++) {
    if (fabs(knownWeightGrams - calibrationLearningKnownWeightsGrams[i]) < 0.01f) {
      Serial.println("That calibration weight was already added. Use a different known weight.");
      showMessage("Duplicate weight", "Use another mass");
      return false;
    }
  }

  if (knownWeightGrams < 50.0f) {
    Serial.println("Small calibration weights are noisy; add a 200g/500g sample before saving.");
  }

  stopFruitDetection(true);

  int32_t averageRaw = 0;
  int32_t minimumRaw = 0;
  int32_t maximumRaw = 0;
  if (!readNau7802AverageRaw(
        CALIBRATION_SAMPLE_READS,
        CALIBRATION_SAMPLE_TIMEOUT_MS,
        averageRaw,
        minimumRaw,
        maximumRaw
      )) {
    Serial.println("Calibration sample ignored: NAU7802 did not provide enough readings.");
    resetWeightState();
    return false;
  }

  const float rawDelta =
    static_cast<float>(averageRaw - loadCell.getZeroOffset());
  Serial.print("Calibration raw zero=");
  Serial.print(loadCell.getZeroOffset());
  Serial.print(" average=");
  Serial.print(averageRaw);
  Serial.print(" delta=");
  Serial.print(rawDelta, 2);
  Serial.print(" min=");
  Serial.print(minimumRaw);
  Serial.print(" max=");
  Serial.println(maximumRaw);
  if (fabs(rawDelta) < 1.0f) {
    Serial.println("Calibration sample ignored: no measurable load-cell change.");
    showMessage("Cal failed", "Check load cell");
    resetWeightState();
    return false;
  }

  const float sampleFactor = rawDelta / knownWeightGrams;
  if (!isValidCalibrationFactor(sampleFactor)) {
    Serial.println("Invalid calibration sample ignored.");
    return false;
  }

  const float measuredAverageGrams = rawDelta / calibration_factor;
  const float measuredSpreadGrams =
    static_cast<float>(maximumRaw - minimumRaw) / fabs(sampleFactor);
  const float allowedSpreadGrams =
    knownWeightGrams * 0.01f > 2.0f ? knownWeightGrams * 0.01f : 2.0f;
  if (measuredSpreadGrams > allowedSpreadGrams) {
    Serial.print("Calibration sample ignored: unstable reading spread=");
    Serial.print(measuredSpreadGrams, 2);
    Serial.print("g allowed=");
    Serial.print(allowedSpreadGrams, 2);
    Serial.println("g");
    showMessage("Cal unstable", "Wait, retry");
    resetWeightState();
    return false;
  }

  calibrationLearningKnownWeightsGrams[calibrationLearningSampleCount] = knownWeightGrams;
  calibrationLearningRawDeltas[calibrationLearningSampleCount] =
    static_cast<int32_t>(rawDelta);
  calibrationLearningWeightRawSum +=
    static_cast<double>(knownWeightGrams) * static_cast<double>(rawDelta);
  calibrationLearningWeightSquaredSum +=
    static_cast<double>(knownWeightGrams) * static_cast<double>(knownWeightGrams);
  calibrationLearningSampleCount++;
  calibrationLearningCandidateFactor = static_cast<float>(
    calibrationLearningWeightRawSum / calibrationLearningWeightSquaredSum
  );

  calibration_factor = calibrationLearningCandidateFactor;
  loadCell.setCalibrationFactor(calibration_factor);
  resetWeightState();

  Serial.print("Calibration sample ");
  Serial.print(calibrationLearningSampleCount);
  Serial.print("/");
  Serial.print(CALIBRATION_REQUIRED_SAMPLES);
  Serial.print(": known=");
  Serial.print(knownWeightGrams, 2);
  Serial.print("g measuredAvg=");
  Serial.print(measuredAverageGrams, 2);
  Serial.print("g spread=");
  Serial.print(measuredSpreadGrams, 2);
  Serial.print("g sampleFactor=");
  Serial.print(sampleFactor, 6);
  Serial.print(" candidate=");
  Serial.println(calibrationLearningCandidateFactor, 6);
  if (calibrationLearningSampleCount == CALIBRATION_REQUIRED_SAMPLES) {
    Serial.println("All five points accepted. Use cal save to store the fitted factor in EEPROM.");
    showMessage("5 points accepted", "Send cal save");
  } else {
    Serial.print("Add another known weight. Samples remaining: ");
    Serial.println(CALIBRATION_REQUIRED_SAMPLES - calibrationLearningSampleCount);
    char progress[21];
    snprintf(progress, sizeof(progress), "Accepted %u/5", calibrationLearningSampleCount);
    showMessage("Cal sample added", progress);
  }
  return true;
}

void saveCalibrationLearning() {
  if (!calibrationLearningActive || !hasCalibrationLearningSamples()) {
    Serial.println("No calibration samples to save.");
    showMessage("No cal samples");
    return;
  }

  if (calibrationLearningSampleCount != CALIBRATION_REQUIRED_SAMPLES) {
    Serial.print("Calibration needs exactly five different weights. Accepted: ");
    Serial.println(calibrationLearningSampleCount);
    char progress[21];
    snprintf(progress, sizeof(progress), "Accepted %u/5", calibrationLearningSampleCount);
    showMessage("Need 5 cal points", progress);
    return;
  }

  saveCalibrationFactor(calibrationLearningCandidateFactor);
  clearCalibrationLearningSession();
  resetWeightState();
  showMessage("Calibration saved");
}

void cancelCalibrationLearning() {
  if (!calibrationLearningActive) {
    Serial.println("No calibration learning session is active.");
    return;
  }

  calibration_factor = calibrationLearningSavedFactor;
  calibrationReady = calibrationLearningSavedReady;
  loadCell.setCalibrationFactor(calibration_factor);
  clearCalibrationLearningSession();
  resetWeightState();
  Serial.print("Calibration learning cancelled. Restored factor: ");
  Serial.println(calibration_factor, 6);
  showMessage("Cal cancelled");
}

void printScaleHelp() {
  Serial.println("Commands:");
  Serial.println("  t = tare / zero");
  Serial.println("  + or a = increase calibration factor");
  Serial.println("  - or z = decrease calibration factor");
  Serial.println("  r = clear saved calibration");
  Serial.println("  cal start = start five-point calibration");
  Serial.println("  cal add 20 = add one of five different known weights");
  Serial.println("  c 500 = same as cal add 500");
  Serial.println("  cal save = save learned calibration factor");
  Serial.println("  cal cancel = restore last saved calibration factor");
  Serial.println("  cal status = show calibration learning status");
  Serial.println("  fruit Mango = set fruit name sent with Firebase sale");
  Serial.println("Serial Monitor: 115200 baud");
  Serial.println("Buttons: released=1, pressed=0");
}

float positiveSensorWeightGrams() {
  if (isnan(lastSensorWeightGrams) || isinf(lastSensorWeightGrams)) {
    return currentWeightGrams;
  }

  const float weight = lastSensorWeightGrams;
  if (weight < NOISE_FLOOR_GRAMS) {
    return 0.0f;
  }
  return weight;
}

float readWeightGrams() {
  if (!nau7802Ready || !calibrationReady) {
    return currentWeightGrams;
  }

  float weight = lastSensorWeightGrams;
  if (isnan(weight) || isinf(weight)) {
    Serial.println("Invalid NAU7802 reading");
    return currentWeightGrams;
  }

  if (weight < 0.0f) weight = 0.0f;
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

float quantizeWeightGrams(float weightGrams, float previousWeightGrams) {
  if (isnan(weightGrams) || isinf(weightGrams) || weightGrams <= 0.0f) {
    return 0.0f;
  }

  const float quantizedWeight =
    round(weightGrams / WEIGHT_DIVISION_GRAMS) * WEIGHT_DIVISION_GRAMS;
  if (previousWeightGrams <= 0.0f) {
    return quantizedWeight;
  }

  const float difference = quantizedWeight - previousWeightGrams;
  if (fabs(difference) > WEIGHT_DIVISION_GRAMS + 0.001f) {
    return quantizedWeight;
  }

  const float halfDivision = WEIGHT_DIVISION_GRAMS * 0.5f;
  if (difference > 0.0f) {
    const float switchUpAt =
      previousWeightGrams + halfDivision + WEIGHT_DIVISION_HYSTERESIS_GRAMS;
    return weightGrams >= switchUpAt ? quantizedWeight : previousWeightGrams;
  }
  if (difference < 0.0f) {
    const float switchDownAt =
      previousWeightGrams - halfDivision - WEIGHT_DIVISION_HYSTERESIS_GRAMS;
    return weightGrams <= switchDownAt ? quantizedWeight : previousWeightGrams;
  }
  return previousWeightGrams;
}

void printScaleDiagnostics() {
  const unsigned long nowMs = millis();
  Serial.print("SCALE status=");
  Serial.print(scaleStatusText());
  Serial.print(" raw=");
  Serial.print(lastNau7802RawReading);
  Serial.print(" zero=");
  Serial.print(loadCell.getZeroOffset());
  Serial.print(" rawDelta=");
  Serial.print(lastNau7802RawReading - loadCell.getZeroOffset());
  Serial.print(" sensor=");
  Serial.print(lastSensorWeightGrams, 2);
  Serial.print("g processed=");
  Serial.print(lastProcessedWeightGrams, 2);
  Serial.print("g filtered=");
  Serial.print(filteredWeightGrams, 2);
  Serial.print("g current=");
  Serial.print(currentWeightGrams, 2);
  Serial.print("g filter=");
  Serial.print(lastFilterAlpha, 2);
  Serial.print(" present=");
  Serial.print(objectPresent ? "yes" : "no");
  Serial.print(" detect=");
  Serial.print(static_cast<unsigned int>(objectDetectCount));
  Serial.print("/");
  Serial.print(static_cast<unsigned int>(OBJECT_CONFIRM_SAMPLES));
  Serial.print(" remove=");
  Serial.print(static_cast<unsigned int>(objectRemoveCount));
  Serial.print("/");
  Serial.print(static_cast<unsigned int>(REMOVE_CONFIRM_SAMPLES));
  Serial.print(" division=");
  Serial.print(WEIGHT_DIVISION_GRAMS, 1);
  Serial.print("g");
  Serial.print(" fruit=");
  Serial.print(currentFruitType);
  Serial.print(" cal=");
  Serial.print(calibration_factor, 6);
  Serial.print(" calibrated=");
  Serial.print(calibrationReady ? "yes" : "no");
  Serial.print(" btnOK=");
  Serial.print(digitalRead(BTN_SUCCESS));
  Serial.print(" btnCancel=");
  Serial.print(digitalRead(BTN_CANCEL));
  Serial.print(" nauAgeMs=");
  Serial.println(nowMs - lastNau7802UpdateMs);
}

void showMessage(const char* line1, const char* line2, unsigned long nowMs) {
  lcd.clear();
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  messageUntilMs = nowMs + MESSAGE_DISPLAY_MS;
}

bool tareScale(const char* reason) {
  if (!nau7802Ready) {
    showMessage("Scale not ready");
    return false;
  }

  Serial.println(reason);
  showMessage("Taring scale...");
  int32_t zeroOffset = 0;
  int32_t minimumRaw = 0;
  int32_t maximumRaw = 0;
  if (!readNau7802AverageRaw(
        TARE_SAMPLE_READS,
        TARE_TIMEOUT_MS,
        zeroOffset,
        minimumRaw,
        maximumRaw
      )) {
    showMessage("Tare failed", "Check scale");
    Serial.println("Tare timeout - check scale stability and wiring");
    return false;
  }

  loadCell.setZeroOffset(zeroOffset);
  Serial.print("NAU7802 zero offset: ");
  Serial.print(zeroOffset);
  Serial.print(" raw spread: ");
  Serial.println(maximumRaw - minimumRaw);
  resetWeightState();
  showMessage("Scale zeroed");
  Serial.println("Tare complete");
  return true;
}

SaleRecord confirmSale(const char* reason, const char* source) {
  if (!calibrationReady) {
    showMessage("Scale uncalibrated", "Run calibration");
    beepBuzzer(2);
    return SaleRecord{};
  }

  if (calibrationLearningActive) {
    showMessage("Calibrating", "No sale saved");
    beepBuzzer(2);
    return SaleRecord{};
  }

  if (currentWeightGrams < OBJECT_DETECT_GRAMS) {
    showMessage("No item on scale");
    beepBuzzer(2);
    return SaleRecord{};
  }

  if (cameraDetectionRequested ||
      strcmp(currentFruitType, "Identifying") == 0 ||
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
  Serial.println(reason);
  stopFruitDetection(false);
  cancelledObjectActive = true;
  currentWeightGrams = 0.0;
  strlcpy(currentFruitType, "Unknown", sizeof(currentFruitType));
  cameraResultReceived = true;
  cameraDetectionRequested = false;
  cameraScanAttemptedForCurrentObject = true;
  showMessage("Cancelled", "Remove item");
  beepBuzzer(2);
}

void updateWeight() {
  if (!nau7802Ready || !newScaleData) return;
  newScaleData = false;

  if (!calibrationReady || calibrationLearningActive) {
    currentWeightGrams = 0.0f;
    lastProcessedWeightGrams = 0.0f;
    filteredWeightGrams = 0.0f;
    lastReturnedWeightGrams = 0.0f;
    return;
  }

  float liveWeightGrams = readWeightGrams();
  const float liveSensorWeightGrams = positiveSensorWeightGrams();

  if (objectRedetectCooldownActive()) {
    stopFruitDetection(true);
    currentWeightGrams = 0.0;
    objectDetectCount = 0;
    objectRemoveCount = 0;
    return;
  }

  if (cancelledObjectActive) {
    currentWeightGrams = 0.0;
    objectPresent = true;
    cameraResultReceived = true;
    cameraDetectionRequested = false;
    cameraScanAttemptedForCurrentObject = true;
    objectDetectCount = 0;

    if (liveSensorWeightGrams <= OBJECT_REMOVE_GRAMS) {
      if (objectRemoveCount < REMOVE_CONFIRM_SAMPLES) objectRemoveCount++;
    } else {
      objectRemoveCount = 0;
    }

    if (objectRemoveCount >= REMOVE_CONFIRM_SAMPLES) {
      resetWeightState();
      startObjectRedetectCooldown();
      Serial.println("Cancelled item removed - zero display resumed");
    }
    return;
  }

  if (objectPresent) {
    if (liveSensorWeightGrams <= OBJECT_REMOVE_GRAMS) {
      if (objectRemoveCount < REMOVE_CONFIRM_SAMPLES) objectRemoveCount++;
      if (objectRemoveCount >= REMOVE_CONFIRM_SAMPLES) {
        resetWeightState();
        startObjectRedetectCooldown();
        Serial.println("Object removed - zero display resumed");
      }
      return;
    }

    objectRemoveCount = 0;
    currentWeightGrams = quantizeWeightGrams(
      liveWeightGrams,
      currentWeightGrams
    );
    if (!cameraResultReceived &&
        !cameraDetectionRequested &&
        !cameraScanAttemptedForCurrentObject &&
        millis() - objectPresentStartedMs >= CAMERA_START_DELAY_MS) {
      requestFruitDetection();
    }
    return;
  }

  if (liveWeightGrams < OBJECT_DETECT_GRAMS ||
      liveSensorWeightGrams <= OBJECT_REMOVE_GRAMS) {
    stopFruitDetection(true);
    objectPresent = false;
    cameraScanAttemptedForCurrentObject = false;
    saleConfirmedForCurrentObject = false;
    cancelledObjectActive = false;
    currentWeightGrams = 0.0;
    objectDetectCount = 0;
    objectRemoveCount = 0;
    objectPresentStartedMs = 0;
    return;
  }

  if (objectDetectCount < OBJECT_CONFIRM_SAMPLES) objectDetectCount++;
  if (objectDetectCount < OBJECT_CONFIRM_SAMPLES) {
    currentWeightGrams = 0.0f;
    return;
  }

  objectPresentStartedMs = millis();
  strlcpy(currentFruitType, "Settling", sizeof(currentFruitType));
  objectPresent = true;
  objectRemoveCount = 0;
  currentWeightGrams = quantizeWeightGrams(liveWeightGrams, 0.0f);
  Serial.print("Object detected, live display(g): ");
  Serial.println(currentWeightGrams, 0);
}

void setupScale() {
  lcd.clear();
  lcd.print("Starting scale...");

  EEPROM.begin(512);
  EEPROM.get(calVal_eepromAddress, calibration_factor);
  uint32_t calibrationMarker = 0;
  EEPROM.get(calVal_eepromAddress + sizeof(calibration_factor), calibrationMarker);
  if (!isValidCalibrationFactor(calibration_factor)) {
    calibration_factor = DEFAULT_CALIBRATION_FACTOR;
    calibrationReady = false;
    persistCalibrationFactor(calibration_factor, false);
  } else {
    calibrationReady = calibrationMarker == CALIBRATION_EEPROM_MAGIC;
    if (!calibrationReady &&
        fabs(calibration_factor - DEFAULT_CALIBRATION_FACTOR) > 0.000001f) {
      calibrationReady = true;
      persistCalibrationFactor(calibration_factor, true);
      Serial.println("Migrated existing NAU7802 calibration factor.");
    }
  }

  Serial.println("Starting NAU7802...");
  Serial.print("I2C SDA GPIO: ");
  Serial.println(I2C_SDA);
  Serial.print("I2C SCL GPIO: ");
  Serial.println(I2C_SCL);
  Serial.print("Calibration factor: ");
  Serial.println(calibration_factor, 6);
  Serial.print("Calibration ready: ");
  Serial.println(calibrationReady ? "yes" : "no");

  if (!loadCell.begin(Wire)) {
    nau7802Ready = false;
    Serial.println("NAU7802 not detected at I2C address 0x2A");
    lcd.clear();
    lcd.print("NAU7802 missing");
    lcd.setCursor(0, 1);
    lcd.print("Check SDA/SCL/GND");
    return;
  }

  if (!loadCell.setChannel(NAU7802_CHANNEL_1) ||
      !loadCell.setGain(NAU7802_GAIN_128) ||
      !loadCell.setSampleRate(NAU7802_SPS_10) ||
      !loadCell.calibrateAFE()) {
    nau7802Ready = false;
    Serial.println("NAU7802 configuration or AFE calibration failed");
    lcd.clear();
    lcd.print("NAU7802 setup fail");
    return;
  }

  loadCell.setCalibrationFactor(calibration_factor);
  nau7802Ready = true;
  currentWeightGrams = 0.0;
  lastNau7802UpdateMs = millis();

  if (!tareScale("Initial NAU7802 tare")) {
    nau7802Ready = false;
    return;
  }

  Serial.println("NAU7802 scale ready on channel 1 at 10 SPS, gain 128");
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
  lcd.print("Please wait");

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
    Serial.println("WiFi connected");
    lcd.clear();
    lcd.print("WiFi connected");
  } else {
    Serial.println("WiFi not connected");
    WiFi.disconnect(false, false);
    lcd.clear();
    lcd.print("WiFi not connected");
    lcd.setCursor(0, 1);
    lcd.print("Offline mode");
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

void removeAcknowledgedSale(const SaleAckPacket& packet) {
  if (!packet.accepted) {
    Serial.println("Worker rejected sale packet; will retry");
    return;
  }

  char firebaseKey[sizeof(packet.firebaseKey)] = {};
  strlcpy(firebaseKey, packet.firebaseKey, sizeof(firebaseKey));
  for (size_t i = 0; i < saleHistoryCount; i++) {
    if (strcmp(saleHistory[i].firebaseKey, firebaseKey) != 0) {
      continue;
    }

    for (size_t j = i + 1; j < saleHistoryCount; j++) {
      saleHistory[j - 1] = saleHistory[j];
    }
    saleHistoryCount--;
    saleHistory[saleHistoryCount] = SaleRecord{};
    Serial.print("Worker accepted sale, pending on scale: ");
    Serial.println(saleHistoryCount);
    return;
  }
}

void onEspNowReceived(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len) {
  if (len == sizeof(PriceUpdatePacket)) {
    PriceUpdatePacket packet = {};
    memcpy(&packet, data, sizeof(packet));
    if (packet.packetType == PACKET_TYPE_PRICE_UPDATE) {
      applyPriceUpdatePacket(packet);
      return;
    }
  }

  if (len == sizeof(SaleAckPacket)) {
    SaleAckPacket packet = {};
    memcpy(&packet, data, sizeof(packet));
    if (packet.packetType == PACKET_TYPE_SALE_ACK) {
      removeAcknowledgedSale(packet);
      return;
    }
  }

  if (len != sizeof(DetectionPacket)) {
    return;
  }

  DetectionPacket packet = {};
  memcpy(&packet, data, sizeof(packet));
  packet.label[sizeof(packet.label) - 1] = '\0';
  if (packet.packetType != PACKET_TYPE_DETECTION_RESULT) {
    return;
  }

  // Ignore detection packets we are no longer waiting for: a result that
  // arrives after a local timeout (or after the item was removed) must not
  // overwrite the current label, otherwise the LCD flickers Unknown -> <fruit>.
  if (!cameraDetectionRequested) {
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
  cameraDetectionRetries = 0;
  strlcpy(currentFruitType, "Identifying", sizeof(currentFruitType));
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
    // The camera always replies within its own window, so reaching here means
    // the START or the result packet was dropped. Keep waiting for the real
    // result: re-send START and reset the timer instead of showing "Unknown".
    if (cameraDetectionRetries < CAMERA_DETECTION_MAX_RETRIES) {
      cameraDetectionRetries++;
      cameraDetectionStartedMs = millis();
      Serial.print("Camera detection timeout - retrying (");
      Serial.print(cameraDetectionRetries);
      Serial.print('/');
      Serial.print(CAMERA_DETECTION_MAX_RETRIES);
      Serial.println(')');
      sendCameraCommand("START");
      return;
    }

    // Camera stayed unresponsive across every retry: now fall back to Unknown.
    strlcpy(currentFruitType, "Unknown", sizeof(currentFruitType));
    cameraDetectionRequested = false;
    cameraResultReceived = true;
    sendCameraCommand("STOP");
    Serial.println("Camera unresponsive after retries - fruit Unknown");
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
  const char* statusText = "Idle";
  if (calibrationLearningActive) {
    statusText = "Calibrating";
  } else if (!calibrationReady) {
    statusText = "Cal needed";
  } else if (objectPresent && !cancelledObjectActive) {
    statusText = "Weighing";
  } else if (objectDetectCount > 0) {
    statusText = "Detecting";
  }

  char line[21];
  char weightText[9];
  formatDisplayWeight(billableWeightGrams, weightText, sizeof(weightText));

  snprintf(line, sizeof(line), "Fruit: %-13.13s", currentFruitType);
  printPadded(0, 0, line);

  if (hasFruitPrice(currentFruitType)) {
    snprintf(line, sizeof(line), "W: %s P:%.2f", weightText, price);
  } else {
    snprintf(line, sizeof(line), "W: %s P:--", weightText);
  }
  printPadded(0, 1, line);

  snprintf(line, sizeof(line), "Status: %s", statusText);
  printPadded(0, 2, line);

  if (rtcReady) {
    DateTime now = rtc.now();
    const char* meridiem = now.hour() >= 12 ? "PM" : "AM";
    uint8_t displayHour = now.hour() % 12;
    if (displayHour == 0) displayHour = 12;
    snprintf(line, sizeof(line), "%02d/%02d/%04d %u:%02d %s",
             now.month(), now.day(), now.year(), displayHour, now.minute(), meridiem);
  } else {
    snprintf(line, sizeof(line), "Date/time: No RTC");
  }
  printPadded(0, 3, line);
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

WiFiClientSecure& sharedTlsClient() {
  static WiFiClientSecure client;
  static bool configured = false;
  if (!configured) {
    client.setInsecure();
    configured = true;
  }
  return client;
}

bool getFirebaseJson(const String& path, String& payload) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure& client = sharedTlsClient();

  HTTPClient http;
  http.setReuse(true);
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
  if (useFirebaseWorkerEsp32) {
    return;
  }

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

  WiFiClientSecure& client = sharedTlsClient();

  HTTPClient http;
  http.setReuse(true);
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

bool sendSaleToFirebaseWorker(const SaleRecord& sale) {
  if (!espNowReady) {
    return false;
  }

  SaleSyncPacket packet = {};
  packet.packetType = PACKET_TYPE_SALE_SYNC;
  packet.sequence = ++saleSyncSequence;
  packet.saleId = static_cast<uint32_t>(sale.id);
  packet.createdAtMs = static_cast<uint32_t>(sale.createdAtMs);
  packet.weightGrams = sale.weightGrams;
  packet.price = sale.price;
  packet.pricePerKg = sale.pricePerKg;
  strlcpy(packet.fruitType, sale.fruitType, sizeof(packet.fruitType));
  strlcpy(packet.timestamp, sale.timestamp, sizeof(packet.timestamp));
  strlcpy(packet.date, sale.date, sizeof(packet.date));
  strlcpy(packet.time, sale.time, sizeof(packet.time));
  strlcpy(packet.source, sale.source, sizeof(packet.source));
  strlcpy(packet.firebaseKey, sale.firebaseKey, sizeof(packet.firebaseKey));

  esp_err_t result = esp_now_send(
    broadcastPeer,
    reinterpret_cast<uint8_t*>(&packet),
    sizeof(packet)
  );
  if (result != ESP_OK) {
    Serial.printf("Worker sale send failed: %d\n", result);
    return false;
  }

  Serial.print("Sale sent to worker: ");
  Serial.println(packet.firebaseKey);
  return true;
}

bool uploadPendingSales() {
  if (saleHistoryCount == 0) {
    return true;
  }

  if (useFirebaseWorkerEsp32) {
    return sendSaleToFirebaseWorker(saleHistory[0]);
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
  if (saleHistoryCount == 0) {
    return true;
  }

  const unsigned long retryMs =
    useFirebaseWorkerEsp32 ? WORKER_SALE_RETRY_MS : FIREBASE_RETRY_MS;
  if ((!useFirebaseWorkerEsp32 &&
       (WiFi.status() != WL_CONNECTED || firebaseUploadBackoffActive())) ||
      millis() - lastFirebaseRetryMs < retryMs) {
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
    if (tareScale("Cancel button tare")) {
      beepBuzzer(2);
    }
  }
}

void processSerialCommand() {
  if (Serial.available() == 0) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.length() == 0) return;
  String lowerCommand = command;
  lowerCommand.toLowerCase();

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
    clearCalibrationLearningSession();
    saveCalibrationFactor(DEFAULT_CALIBRATION_FACTOR, false);
    resetWeightState();
    showMessage("Calibration reset", "Tare and calibrate");
    return;
  }

  if (lowerCommand == "cal start") {
    beginCalibrationLearning();
    return;
  }

  if (lowerCommand == "cal save") {
    saveCalibrationLearning();
    return;
  }

  if (lowerCommand == "cal cancel") {
    cancelCalibrationLearning();
    return;
  }

  if (lowerCommand == "cal status") {
    printCalibrationLearningStatus();
    return;
  }

  if (lowerCommand.startsWith("cal add ")) {
    addCalibrationLearningSample(command.substring(8).toFloat());
    return;
  }

  if (lowerCommand.startsWith("cal ")) {
    addCalibrationLearningSample(command.substring(4).toFloat());
    return;
  }

  if (command[0] == 'c' || command[0] == 'C') {
    float knownWeightGrams = command.substring(1).toFloat();
    addCalibrationLearningSample(knownWeightGrams);
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

  if (!nau7802Ready) {
    showMessage("Scale not ready");
  } else if (!calibrationReady) {
    showMessage("Calibration needed", "Tare, then c 500");
  } else {
    showMessage("Ready!");
  }
}

void loop() {
  if (captureNau7802Reading()) {
    newScaleData = true;
  }

  if (nau7802Ready) {
    updateWeight();
  }

  handleButtons();
  processSerialCommand();
  maintainFruitDetection();
  maintainPriceUpdates();
  maintainPendingSaleUpload();

  if (millis() - lastSerialDiagnosticMs >= SERIAL_DIAGNOSTIC_INTERVAL_MS) {
    printScaleDiagnostics();
    if (nau7802Ready && millis() - lastNau7802UpdateMs > 2000) {
      Serial.println("NAU7802 not updating - check SDA/SCL, common GND, and power.");
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
