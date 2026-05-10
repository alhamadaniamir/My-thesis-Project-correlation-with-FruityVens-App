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

// HX711 pins
#define HX_DOUT 23
#define HX_SCK 22

// I2C pins
#define I2C_SDA 18
#define I2C_SCL 21

// Buttons
#define BTN_SUCCESS 12
#define BTN_CANCEL 14

RTC_DS3231 rtc;

// Wi-Fi
const char* ssid = "Aida_iPhone";
const char* password = "1234567899";

// NTP
const char* ntpServer = "time.google.com";
const long gmtOffset_sec = 8 * 3600;
const int daylightOffset_sec = 0;

const int LCD_COLS = 20;
const int LCD_ROWS = 4;
const float DEFAULT_CALIBRATION_FACTOR = 2280.0;
const unsigned long BUTTON_ACTION_COOLDOWN_MS = 300;
const unsigned long BUTTON_DEBOUNCE_MS = 80;
const unsigned long MESSAGE_DISPLAY_MS = 900;
const bool ENABLE_BUTTON_ACTIONS = true;

hd44780_I2Cexp lcd(0x27);
HX711_ADC LoadCell(HX_DOUT, HX_SCK);

const int calVal_eepromAddress = 0;
float calibration_factor = 1.0;
float pricePerKg = 60.0;
unsigned long lastDisplayMs = 0;
unsigned long lastHx711UpdateMs = 0;
unsigned long lastButtonDebugMs = 0;
unsigned long messageUntilMs = 0;
float currentWeightGrams = 0.0;
float currentScaleReading = 0.0;
long hx711UpdateCount = 0;

bool rtcReady = false;
bool hx711Ready = false;
bool newWeightReady = false;
bool tareInProgress = false;
unsigned long lastSuccessActionMs = 0;
unsigned long lastCancelActionMs = 0;
unsigned long tareStartMs = 0;

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

float readWeightGrams() {
  float w = LoadCell.getData();
  if (isnan(w) || isinf(w)) {
    Serial.println("Invalid HX711 weight value, showing 0.0g");
    return 0.0;
  }
  currentScaleReading = w;
  return round(w * 100.0) / 100.0;
}

void printScaleHelp() {
  Serial.println("HX711 commands:");
  Serial.println("  t = tare/zero the scale");
  Serial.println("  c 500 = calibrate using a known 500g weight");
  Serial.println("  r = reset calibration to default");
  Serial.println("  d = diagnostic factor 1.0");
  Serial.println("Calibration: remove load, send t, place known weight, then send c <grams>.");
}

bool isValidCalibrationFactor(float value) {
  return !isnan(value) && !isinf(value) && fabs(value) >= 0.1 && fabs(value) <= 1000000.0;
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

bool saveCalibrationFactor(float newCalibrationFactor) {
  if (!isValidCalibrationFactor(newCalibrationFactor)) {
    Serial.println("Invalid calibration result. Not saved.");
    Serial.println("Remove load, send t, place known weight, then send c <grams> again.");
    return false;
  }

  calibration_factor = newCalibrationFactor;
  LoadCell.setCalFactor(calibration_factor);
  EEPROM.put(calVal_eepromAddress, calibration_factor);
  EEPROM.commit();
  Serial.print("Saved calibration factor: ");
  Serial.println(calibration_factor, 6);
  return true;
}

void startTare(const char* reason) {
  if (!hx711Ready) return;

  Serial.println(reason);
  LoadCell.tareNoDelay();
  tareStartMs = millis();
  tareInProgress = true;
}

void updateTareStatus() {
  if (!tareInProgress) return;

  if (LoadCell.getTareStatus()) {
    currentWeightGrams = 0;
    tareInProgress = false;
    Serial.println("Tare complete");
    lcd.clear();
    lcd.print("Scale zeroed");
    messageUntilMs = millis() + MESSAGE_DISPLAY_MS;
    return;
  }

  if (millis() - tareStartMs > 5000) {
    tareInProgress = false;
    Serial.println("Tare timeout - HX711 did not finish tare");
    lcd.clear();
    lcd.print("Tare timeout");
    messageUntilMs = millis() + MESSAGE_DISPLAY_MS;
  }
}

void processSerialCommand() {
  if (!hx711Ready || Serial.available() == 0) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.length() == 0) return;

  if (command == "t" || command == "T") {
    startTare("Serial tare requested");
    return;
  }

  if (command == "r" || command == "R") {
    if (saveCalibrationFactor(DEFAULT_CALIBRATION_FACTOR)) {
      currentWeightGrams = 0.0;
      Serial.println("Calibration reset to default.");
    }
    return;
  }

  if (command == "d" || command == "D") {
    if (saveCalibrationFactor(1.0)) {
      currentWeightGrams = readWeightGrams();
      Serial.println("Diagnostic calibration factor set to 1.0.");
      Serial.println("This is not grams, but it should visibly change when force is applied.");
    }
    return;
  }

  if (command[0] == 'c' || command[0] == 'C') {
    float knownWeightGrams = command.substring(1).toFloat();
    if (knownWeightGrams <= 0) {
      Serial.println("Calibration command needs grams, example: c 500");
      return;
    }

    float newCalibrationFactor = LoadCell.getNewCalibration(knownWeightGrams);
    if (saveCalibrationFactor(newCalibrationFactor)) {
      currentWeightGrams = readWeightGrams();
      Serial.print("Calibrated with known weight(g): ");
      Serial.println(knownWeightGrams, 1);
    }
    return;
  }

  printScaleHelp();
}

void setupScale() {
  lcd.clear();
  lcd.print("Starting scale...");

  Serial.println("Starting HX711...");
  Serial.print("HX711 DOUT GPIO: ");
  Serial.println(HX_DOUT);
  Serial.print("HX711 SCK GPIO: ");
  Serial.println(HX_SCK);
  Serial.println("Remove all weight before startup tare.");

  EEPROM.begin(512);
  EEPROM.get(calVal_eepromAddress, calibration_factor);
  if (!isValidCalibrationFactor(calibration_factor)) {
    Serial.println("Invalid EEPROM calibration, using default calibration factor");
    calibration_factor = DEFAULT_CALIBRATION_FACTOR;
    EEPROM.put(calVal_eepromAddress, calibration_factor);
    EEPROM.commit();
  }

  Serial.print("Calibration factor: ");
  Serial.println(calibration_factor);

  LoadCell.begin();
  LoadCell.setSamplesInUse(4);

  unsigned long stabilizingtime = 2000;
  boolean tare = true;
  LoadCell.start(stabilizingtime, tare);

  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
    Serial.println("HX711 timeout - check wiring");
    Serial.print("DOUT pin level: ");
    Serial.println(digitalRead(HX_DOUT));
    lcd.clear();
    lcd.print("HX711 timeout");
    lcd.setCursor(0, 1);
    lcd.print("DOUT=");
    lcd.print(digitalRead(HX_DOUT));
    lcd.print(" SCK=");
    lcd.print(HX_SCK);
    hx711Ready = false;
    return;
  }

  LoadCell.setCalFactor(calibration_factor);
  lastHx711UpdateMs = millis();
  hx711Ready = true;
  Serial.println("Scale ready");
  printScaleHelp();
}

void setupRtc() {
  if (!rtc.begin()) {
    Serial.println("RTC NOT FOUND!");
    rtcReady = false;
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

void printPadded(uint8_t col, uint8_t row, const char* text) {
  lcd.setCursor(col, row);
  lcd.print(text);
  for (uint8_t i = strlen(text); i < LCD_COLS - col; i++) {
    lcd.print(' ');
  }
}

void updateDisplay(float weightGrams) {
  float billableWeightGrams = weightGrams;
  if (billableWeightGrams < 0) billableWeightGrams = 0;
  float price = (billableWeightGrams / 1000.0) * pricePerKg;
  bool successRawReleased = digitalRead(BTN_SUCCESS) == HIGH;
  bool cancelRawReleased = digitalRead(BTN_CANCEL) == HIGH;
  char line[21];

  snprintf(line, sizeof(line), "Weight:%8.2f g", weightGrams);
  printPadded(0, 0, line);

  if (!successRawReleased || !cancelRawReleased) {
    snprintf(line, sizeof(line), "Btn S:%d C:%d", successRawReleased, cancelRawReleased);
  } else if (!hx711Ready) {
    snprintf(line, sizeof(line), "HX711 startup failed");
  } else if (tareInProgress) {
    snprintf(line, sizeof(line), "Taring scale...");
  } else if (millis() - lastHx711UpdateMs > 2000) {
    snprintf(line, sizeof(line), "No data DOUT:%d", digitalRead(HX_DOUT));
  } else {
    snprintf(line, sizeof(line), "Scale ready");
  }
  printPadded(0, 1, line);

  snprintf(line, sizeof(line), "Price: PHP %7.2f", price);
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

void handleButtons(float weightGrams) {
  if (!ENABLE_BUTTON_ACTIONS) {
    updateButton(successButton);
    updateButton(cancelButton);
    return;
  }

  bool successPressed = updateButton(successButton);
  bool cancelPressed = updateButton(cancelButton);
  unsigned long nowMs = millis();

  if (successPressed && nowMs - lastSuccessActionMs >= BUTTON_ACTION_COOLDOWN_MS) {
    lastSuccessActionMs = nowMs;
    float billableWeightGrams = weightGrams;
    if (billableWeightGrams < 0) billableWeightGrams = 0;
    float price = (billableWeightGrams / 1000.0) * pricePerKg;
    Serial.print("SALE OK - Weight(g): ");
    Serial.print(weightGrams, 2);
    Serial.print(" Price: ");
    Serial.println(price, 2);
    lcd.clear();
    lcd.print("Sale confirmed");
    lcd.setCursor(0, 1);
    lcd.print("Total PHP ");
    lcd.print(price, 2);
    messageUntilMs = nowMs + MESSAGE_DISPLAY_MS;
  }

  if (cancelPressed && nowMs - lastCancelActionMs >= BUTTON_ACTION_COOLDOWN_MS) {
    lastCancelActionMs = nowMs;
    lcd.clear();
    lcd.print("Cancelled");
    if (hx711Ready && !tareInProgress) {
      lcd.setCursor(0, 1);
      lcd.print("Taring scale...");
      startTare("Cancelled - tare scale");
      messageUntilMs = nowMs + MESSAGE_DISPLAY_MS;
    } else {
      lcd.setCursor(0, 1);
      lcd.print("Scale not ready");
      messageUntilMs = nowMs + MESSAGE_DISPLAY_MS;
    }
  }

}

void setup() {
  Serial.begin(57600);
  Serial.setTimeout(50);
  lastSuccessActionMs = millis() - BUTTON_ACTION_COOLDOWN_MS;
  lastCancelActionMs = millis() - BUTTON_ACTION_COOLDOWN_MS;

  pinMode(BTN_SUCCESS, INPUT_PULLUP);
  pinMode(BTN_CANCEL, INPUT_PULLUP);
  delay(20);
  beginButton(successButton);
  beginButton(cancelButton);

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

  lcd.clear();
  if (hx711Ready) {
    lcd.print("Ready!");
  } else {
    lcd.print("Scale not ready");
  }
  delay(1000);
  lcd.clear();
}

void loop() {
  if (hx711Ready && LoadCell.update()) {
    lastHx711UpdateMs = millis();
    currentWeightGrams = readWeightGrams();
    hx711UpdateCount++;
    newWeightReady = true;
  }

  handleButtons(currentWeightGrams);

  processSerialCommand();
  updateTareStatus();

  if (millis() - lastDisplayMs >= 500) {
    Serial.print("Weight(g): ");
    Serial.println(currentWeightGrams, 4);
    Serial.print("Scale reading: ");
    Serial.println(currentScaleReading, 6);
    Serial.print("Calibration factor: ");
    Serial.println(calibration_factor, 6);
    Serial.print("HX711 update count: ");
    Serial.println(hx711UpdateCount);

    if (newWeightReady) {
      Serial.println("New HX711 data received");
      newWeightReady = false;
    } else {
      Serial.print("No new HX711 data. DOUT pin level: ");
      Serial.println(digitalRead(HX_DOUT));
    }

    if (!hx711Ready || millis() - lastHx711UpdateMs > 2000) {
      Serial.println("HX711 not updating - check DOUT/SCK wiring, power, and shared ground.");
    }

    if (millis() >= messageUntilMs) {
      updateDisplay(currentWeightGrams);
    }
    lastDisplayMs = millis();
  }

  if (millis() - lastButtonDebugMs >= 2000) {
    Serial.print("Buttons raw SUCCESS=");
    Serial.print(digitalRead(BTN_SUCCESS));
    Serial.print(" CANCEL=");
    Serial.println(digitalRead(BTN_CANCEL));
    lastButtonDebugMs = millis();
  }
}
