#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <HX711_ADC.h>
#include <EEPROM.h>
#include <WiFi.h>
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

const char* ssid = "Aida_iPhone";
const char* password = "1234567899";

const char* ntpServer = "time.google.com";
const long gmtOffset_sec = 8 * 3600;
const int daylightOffset_sec = 0;

const int LCD_COLS = 20;
const int LCD_ROWS = 4;
const int calVal_eepromAddress = 0;

const float OLD_DEFAULT_CALIBRATION_FACTOR = 1.0;
const float DEFAULT_CALIBRATION_FACTOR = 0.1012;
const float pricePerKg = 60.0;
const unsigned long DISPLAY_INTERVAL_MS = 1000;
const unsigned long BUTTON_DEBOUNCE_MS = 80;
const unsigned long BUTTON_COOLDOWN_MS = 300;
const unsigned long MESSAGE_DISPLAY_MS = 1000;
const unsigned long STABLE_LOCK_MS = 900;
const float OBJECT_DETECT_GRAMS = 5.0;
const float OBJECT_REMOVE_GRAMS = 2.0;
const float STABLE_DELTA_GRAMS = 1.5;
const float NOISE_FLOOR_GRAMS = 2.0;
const float WEIGHT_FILTER_ALPHA = 0.25;
const uint8_t OBJECT_CONFIRM_SAMPLES = 3;
const uint8_t REMOVE_CONFIRM_SAMPLES = 4;

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
unsigned long lastHx711UpdateMs = 0;
unsigned long lastSuccessActionMs = 0;
unsigned long lastCancelActionMs = 0;
unsigned long messageUntilMs = 0;
unsigned long stableSinceMs = 0;
float lastLiveWeightGrams = 0.0;
float lockedWeightGrams = 0.0;
float filteredWeightGrams = 0.0;
bool hx711Ready = false;
bool rtcReady = false;
bool weightLocked = false;
bool objectPresent = false;
bool newScaleData = false;
uint8_t objectDetectCount = 0;
uint8_t objectRemoveCount = 0;

bool isValidCalibrationFactor(float value) {
  return !isnan(value) && !isinf(value) && fabs(value) >= 0.1 && fabs(value) <= 1000000.0;
}

void printPadded(uint8_t col, uint8_t row, const char* text) {
  lcd.setCursor(col, row);
  lcd.print(text);
  for (uint8_t i = strlen(text); i < LCD_COLS - col; i++) {
    lcd.print(' ');
  }
}

void beginButton(ButtonState &button) {
  button.rawPressed = digitalRead(button.pin) == LOW;
  button.stablePressed = button.rawPressed;
  button.previousStablePressed = button.stablePressed;
  button.armed = !button.stablePressed;
  button.lastRawChangeMs = millis();
}

bool updateButton(ButtonState &button) {
  bool rawPressed = digitalRead(button.pin) == LOW;

  if (rawPressed != button.rawPressed) {
    button.rawPressed = rawPressed;
    button.lastRawChangeMs = millis();
  }

  button.previousStablePressed = button.stablePressed;
  if (millis() - button.lastRawChangeMs >= BUTTON_DEBOUNCE_MS) {
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

void saveCalibrationFactor(float value) {
  if (!isValidCalibrationFactor(value)) {
    Serial.println("Invalid calibration factor, not saved.");
    return;
  }

  calibration_factor = value;
  LoadCell.setCalFactor(calibration_factor);
  EEPROM.put(calVal_eepromAddress, calibration_factor);
  EEPROM.commit();
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
  Serial.println("Buttons: released=1, pressed=0");
}

float readWeightGrams() {
  if (!hx711Ready) {
    return currentWeightGrams;
  }

  float weight = LoadCell.getData();
  if (isnan(weight) || isinf(weight)) {
    Serial.println("Invalid HX711 reading");
    return currentWeightGrams;
  }

  weight = fabs(weight);
  if (weight < NOISE_FLOOR_GRAMS) weight = 0;

  filteredWeightGrams =
    (WEIGHT_FILTER_ALPHA * weight) + ((1.0 - WEIGHT_FILTER_ALPHA) * filteredWeightGrams);
  if (filteredWeightGrams < NOISE_FLOOR_GRAMS) filteredWeightGrams = 0;

  return round(filteredWeightGrams * 10.0) / 10.0;
}

void showMessage(const char* line1, const char* line2 = "") {
  lcd.clear();
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  messageUntilMs = millis() + MESSAGE_DISPLAY_MS;
}

void tareScale(const char* reason) {
  if (!hx711Ready) {
    showMessage("Scale not ready");
    return;
  }

  Serial.println(reason);
  showMessage("Taring scale...");
  LoadCell.tareNoDelay();
  while (!LoadCell.getTareStatus()) {
    LoadCell.update();
  }
  weightLocked = false;
  objectPresent = false;
  lockedWeightGrams = 0.0;
  lastLiveWeightGrams = 0.0;
  filteredWeightGrams = 0.0;
  objectDetectCount = 0;
  objectRemoveCount = 0;
  stableSinceMs = 0;
  currentWeightGrams = 0.0;
  showMessage("Scale zeroed");
  Serial.println("Tare complete");
}

void updateLockedWeight() {
  if (!hx711Ready || !newScaleData) return;
  newScaleData = false;

  float liveWeightGrams = readWeightGrams();
  unsigned long nowMs = millis();

  if (weightLocked) {
    currentWeightGrams = lockedWeightGrams;
    if (liveWeightGrams <= OBJECT_REMOVE_GRAMS) {
      if (objectRemoveCount < REMOVE_CONFIRM_SAMPLES) objectRemoveCount++;
    } else {
      objectRemoveCount = 0;
    }

    if (objectRemoveCount >= REMOVE_CONFIRM_SAMPLES) {
      weightLocked = false;
      objectPresent = false;
      lockedWeightGrams = 0.0;
      currentWeightGrams = 0.0;
      stableSinceMs = 0;
      lastLiveWeightGrams = 0.0;
      filteredWeightGrams = 0.0;
      objectDetectCount = 0;
      objectRemoveCount = 0;
      Serial.println("Object removed - zero display resumed");
    }
    return;
  }

  if (liveWeightGrams < OBJECT_DETECT_GRAMS) {
    objectPresent = false;
    currentWeightGrams = 0.0;
    stableSinceMs = 0;
    lastLiveWeightGrams = liveWeightGrams;
    objectDetectCount = 0;
    objectRemoveCount = 0;
    return;
  }

  if (objectDetectCount < OBJECT_CONFIRM_SAMPLES) {
    objectDetectCount++;
    currentWeightGrams = 0.0;
    lastLiveWeightGrams = liveWeightGrams;
    return;
  }

  objectPresent = true;
  objectRemoveCount = 0;
  currentWeightGrams = liveWeightGrams;
  if (fabs(liveWeightGrams - lastLiveWeightGrams) <= STABLE_DELTA_GRAMS) {
    if (stableSinceMs == 0) {
      stableSinceMs = nowMs;
    }

    if (nowMs - stableSinceMs >= STABLE_LOCK_MS) {
      lockedWeightGrams = liveWeightGrams;
      currentWeightGrams = lockedWeightGrams;
      weightLocked = true;
      Serial.print("Weight locked(g): ");
      Serial.println(lockedWeightGrams, 1);
    }
  } else {
    stableSinceMs = nowMs;
  }

  lastLiveWeightGrams = liveWeightGrams;
}

void setupScale() {
  lcd.clear();
  lcd.print("Starting scale...");

  EEPROM.begin(512);
  EEPROM.get(calVal_eepromAddress, calibration_factor);
  if (!isValidCalibrationFactor(calibration_factor)) {
    calibration_factor = DEFAULT_CALIBRATION_FACTOR;
    EEPROM.put(calVal_eepromAddress, calibration_factor);
    EEPROM.commit();
  } else if (fabs(calibration_factor - OLD_DEFAULT_CALIBRATION_FACTOR) < 0.0001) {
    calibration_factor = DEFAULT_CALIBRATION_FACTOR;
    EEPROM.put(calVal_eepromAddress, calibration_factor);
    EEPROM.commit();
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

  WiFi.begin(ssid, password);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi OK");
  } else {
    Serial.println("WiFi FAILED");
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

void updateDisplay() {
  float billableWeightGrams = currentWeightGrams;
  if (billableWeightGrams < 0) billableWeightGrams = 0;
  float price = (billableWeightGrams / 1000.0) * pricePerKg;

  char line[21];

  snprintf(line, sizeof(line), "Weight:%8.2f g", currentWeightGrams);
  printPadded(0, 0, line);

  snprintf(line, sizeof(line), "Price: PHP %7.2f", price);
  printPadded(0, 1, line);

  if (weightLocked) {
    snprintf(line, sizeof(line), "Status: locked");
  } else if (objectPresent) {
    snprintf(line, sizeof(line), "Status: weighing");
  } else {
    snprintf(line, sizeof(line), "Status: zero");
  }
  printPadded(0, 2, line);

  if (rtcReady) {
    DateTime now = rtc.now();
    snprintf(line, sizeof(line), "%02d/%02d/%04d %02d:%02d",
             now.month(), now.day(), now.year(), now.hour(), now.minute());
  } else {
    snprintf(line, sizeof(line), "RTC not detected");
  }
  printPadded(0, 3, line);
}

void handleButtons() {
  bool successPressed = updateButton(successButton);
  bool cancelPressed = updateButton(cancelButton);
  unsigned long nowMs = millis();

  if (successPressed && nowMs - lastSuccessActionMs >= BUTTON_COOLDOWN_MS) {
    lastSuccessActionMs = nowMs;
    float price = (currentWeightGrams / 1000.0) * pricePerKg;
    if (price < 0) price = 0;

    Serial.print("SALE OK - Weight(g): ");
    Serial.print(currentWeightGrams, 2);
    Serial.print(" Price: ");
    Serial.println(price, 2);

    char line2[21];
    snprintf(line2, sizeof(line2), "PHP %.2f", price);
    showMessage("Sale confirmed", line2);
  }

  if (cancelPressed && nowMs - lastCancelActionMs >= BUTTON_COOLDOWN_MS) {
    lastCancelActionMs = nowMs;
    tareScale("Cancel button tare");
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
    saveCalibrationFactor(calibration_factor + 1.0);
    return;
  }

  if (command == "-" || command == "z" || command == "Z") {
    saveCalibrationFactor(calibration_factor - 1.0);
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
    weightLocked = false;
    objectPresent = false;
    lockedWeightGrams = 0.0;
    currentWeightGrams = readWeightGrams();
    return;
  }

  printScaleHelp();
}

void setup() {
  Serial.begin(57600);
  Serial.setTimeout(50);

  pinMode(BTN_SUCCESS, INPUT_PULLUP);
  pinMode(BTN_CANCEL, INPUT_PULLUP);
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
  syncRtcFromNtp();
  setupScale();

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

  if (millis() - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    if (objectPresent || weightLocked) {
      Serial.print("Weight(g): ");
      Serial.println(currentWeightGrams, 2);
    } else {
      Serial.println("Weight(g): 0.00");
    }
    Serial.print("Calibration factor: ");
    Serial.println(calibration_factor, 6);
    if (hx711Ready && millis() - lastHx711UpdateMs > 2000) {
      Serial.println("HX711 not updating - check DOUT/SCK wiring and power.");
    }
    Serial.print("Buttons raw SUCCESS=");
    Serial.print(digitalRead(BTN_SUCCESS));
    Serial.print(" CANCEL=");
    Serial.println(digitalRead(BTN_CANCEL));

    if (millis() >= messageUntilMs) {
      updateDisplay();
    }
    lastDisplayMs = millis();
  }
}
