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

hd44780_I2Cexp lcd(0x27);
HX711_ADC LoadCell(HX_DOUT, HX_SCK);

const int calVal_eepromAddress = 0;
float calibration_factor = 1.0;
float pricePerKg = 60.0;
unsigned long lastDisplayMs = 0;
unsigned long lastHx711UpdateMs = 0;
float currentWeightGrams = 0.0;

bool successWasPressed = false;
bool cancelWasPressed = false;
bool rtcReady = false;
bool hx711Ready = false;
bool newWeightReady = false;

float readWeightGrams() {
  float w = LoadCell.getData();
  if (fabs(w) < 1.0) w = 0;
  return round(w * 10.0) / 10.0;
}

float readSignalValue(float weightGrams) {
  return weightGrams * calibration_factor;
}

void setupScale() {
  lcd.clear();
  lcd.print("Starting scale...");

  Serial.println("Starting HX711...");
  Serial.print("HX711 DOUT GPIO: ");
  Serial.println(HX_DOUT);
  Serial.print("HX711 SCK GPIO: ");
  Serial.println(HX_SCK);

  EEPROM.begin(512);
  EEPROM.get(calVal_eepromAddress, calibration_factor);
  if (isnan(calibration_factor) || fabs(calibration_factor) < 0.1 || fabs(calibration_factor) > 1000000.0) {
    Serial.println("Invalid EEPROM calibration, using diagnostic factor 1.0");
    calibration_factor = 1.0;
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
  float signalValue = readSignalValue(weightGrams);

  char line[21];

  snprintf(line, sizeof(line), "Weight:%8.1f g", weightGrams);
  printPadded(0, 0, line);

  if (!hx711Ready) {
    snprintf(line, sizeof(line), "HX711 startup failed");
  } else if (millis() - lastHx711UpdateMs > 2000) {
    snprintf(line, sizeof(line), "No data DOUT:%d", digitalRead(HX_DOUT));
  } else {
    snprintf(line, sizeof(line), "Signal:%8.0f", signalValue);
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
  bool successPressed = digitalRead(BTN_SUCCESS) == LOW;
  bool cancelPressed = digitalRead(BTN_CANCEL) == LOW;

  if (successPressed && cancelPressed) {
    Serial.println("Both buttons detected - check wiring");
    lcd.clear();
    lcd.print("Both buttons ON");
    lcd.setCursor(0, 1);
    lcd.print("Check GPIO 12/14");
    delay(1200);
    lcd.clear();
  }

  if (successPressed && !cancelPressed && !successWasPressed) {
    float billableWeightGrams = weightGrams;
    if (billableWeightGrams < 0) billableWeightGrams = 0;
    float price = (billableWeightGrams / 1000.0) * pricePerKg;
    Serial.print("SALE OK - Weight(g): ");
    Serial.print(weightGrams, 1);
    Serial.print(" Price: ");
    Serial.println(price, 2);

    lcd.clear();
    lcd.print("Sale confirmed");
    lcd.setCursor(0, 1);
    lcd.print("Total PHP ");
    lcd.print(price, 2);
    delay(1200);
    lcd.clear();
  }

  if (cancelPressed && !successPressed && !cancelWasPressed) {
    if (!hx711Ready) {
      Serial.println("Cannot tare - HX711 is not ready");
      lcd.clear();
      lcd.print("HX711 not ready");
      delay(1200);
      lcd.clear();
      successWasPressed = successPressed;
      cancelWasPressed = cancelPressed;
      return;
    }

    Serial.println("Cancelled - tare scale");
    lcd.clear();
    lcd.print("Cancelled");
    lcd.setCursor(0, 1);
    lcd.print("Taring scale...");
    LoadCell.tareNoDelay();
    unsigned long tareStartMs = millis();
    while (!LoadCell.getTareStatus()) {
      LoadCell.update();
      if (millis() - tareStartMs > 5000) {
        Serial.println("Tare timeout - HX711 did not finish tare");
        lcd.setCursor(0, 2);
        lcd.print("Tare timeout");
        delay(1200);
        lcd.clear();
        successWasPressed = successPressed;
        cancelWasPressed = cancelPressed;
        return;
      }
    }
    lcd.setCursor(0, 2);
    lcd.print("Scale zeroed");
    delay(1200);
    lcd.clear();
  }

  successWasPressed = successPressed;
  cancelWasPressed = cancelPressed;
}

void setup() {
  Serial.begin(57600);

  pinMode(BTN_SUCCESS, INPUT_PULLUP);
  pinMode(BTN_CANCEL, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);

  int lcdStatus = lcd.begin(LCD_COLS, LCD_ROWS);
  if (lcdStatus) {
    hd44780::fatalError(lcdStatus);
  }
  lcd.backlight();
  lcd.print("Initializing...");

  setupScale();

  setupRtc();

  setupWifi();
  syncRtcFromNtp();

  lcd.clear();
  lcd.print("Ready!");
  delay(1000);
  lcd.clear();
}

void loop() {
  if (hx711Ready && LoadCell.update()) {
    lastHx711UpdateMs = millis();
    currentWeightGrams = readWeightGrams();
    newWeightReady = true;
  }

  handleButtons(currentWeightGrams);

  if (millis() - lastDisplayMs >= 500) {
    Serial.print("Weight(g): ");
    Serial.println(currentWeightGrams, 1);
    if (newWeightReady) {
      Serial.print("Signal estimate: ");
      Serial.println(readSignalValue(currentWeightGrams), 0);
      newWeightReady = false;
    } else {
      Serial.print("No new HX711 data. DOUT pin level: ");
      Serial.println(digitalRead(HX_DOUT));
    }
    if (!hx711Ready || millis() - lastHx711UpdateMs > 2000) {
      Serial.println("HX711 not updating - check DOUT/SCK wiring, power, and shared ground.");
    }
    updateDisplay(currentWeightGrams);
    lastDisplayMs = millis();
  }
}
