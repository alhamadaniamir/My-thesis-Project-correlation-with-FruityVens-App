#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <HX711_ADC.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>
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
const float pricePerKg = 60.0;
const unsigned long DISPLAY_INTERVAL_MS = 300;
const unsigned long BUTTON_DEBOUNCE_MS = 80;
const unsigned long BUTTON_COOLDOWN_MS = 300;
const unsigned long MESSAGE_DISPLAY_MS = 1000;
const unsigned long TARE_TIMEOUT_MS = 5000;
const unsigned int BUZZER_BEEP_MS = 220;
const unsigned int BUZZER_PAUSE_MS = 120;
const float OBJECT_DETECT_GRAMS = 5.0;
const float OBJECT_REMOVE_GRAMS = 2.0;
const float NOISE_FLOOR_GRAMS = 2.0;
const float WEIGHT_FILTER_ALPHA = 0.45;
const float FAST_WEIGHT_FILTER_ALPHA = 0.85;
const float FAST_WEIGHT_DELTA_GRAMS = 20.0;
const uint8_t OBJECT_CONFIRM_SAMPLES = 1;
const uint8_t REMOVE_CONFIRM_SAMPLES = 2;
const uint8_t LOCK_MATCH_SAMPLES = 10;
const size_t SALE_HISTORY_SIZE = 10;

struct SaleRecord {
  unsigned long id;
  float weightGrams;
  float price;
  char fruitType[32];
  char timestamp[25];
  char date[11];
  char time[9];
  char source[16];
};

hd44780_I2Cexp lcd(0x27);
HX711_ADC LoadCell(HX_DOUT, HX_SCK);
RTC_DS3231 rtc;
WebServer server(80);

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
float lastLockCandidateGrams = 0.0;
float lockedWeightGrams = 0.0;
float filteredWeightGrams = 0.0;
bool hx711Ready = false;
bool rtcReady = false;
bool weightLocked = false;
bool objectPresent = false;
bool newScaleData = false;
uint8_t objectDetectCount = 0;
uint8_t objectRemoveCount = 0;
uint8_t lockMatchCount = 0;
unsigned long nextSaleId = 1;
unsigned long latestSaleId = 0;
size_t saleHistoryCount = 0;
SaleRecord saleHistory[SALE_HISTORY_SIZE];
char currentFruitType[32] = "Unknown";

SaleRecord recordSale(const char* source);
String saleRecordJson(const SaleRecord& sale);

float calculatePrice(float weightGrams) {
  if (weightGrams < 0) weightGrams = 0;
  return (weightGrams / 1000.0) * pricePerKg;
}

const char* scaleStatusText() {
  if (weightLocked) return "locked";
  if (objectPresent) return "weighing";
  return "zero";
}

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void sendJson(int statusCode, const String& body) {
  sendCorsHeaders();
  server.send(statusCode, "application/json", body);
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

void handleOptions() {
  sendCorsHeaders();
  server.send(204);
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
  weightLocked = false;
  objectPresent = false;
  lockedWeightGrams = 0.0;
  currentWeightGrams = 0.0;
  lastLockCandidateGrams = 0.0;
  filteredWeightGrams = 0.0;
  objectDetectCount = 0;
  objectRemoveCount = 0;
  lockMatchCount = 0;
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

  if (filteredWeightGrams == 0.0 && weight >= OBJECT_DETECT_GRAMS) {
    filteredWeightGrams = weight;
    return round(filteredWeightGrams * 10.0) / 10.0;
  }

  float filterAlpha = WEIGHT_FILTER_ALPHA;
  if (fabs(weight - filteredWeightGrams) >= FAST_WEIGHT_DELTA_GRAMS) {
    filterAlpha = FAST_WEIGHT_FILTER_ALPHA;
  }

  filteredWeightGrams =
    (filterAlpha * weight) + ((1.0 - filterAlpha) * filteredWeightGrams);
  if (filteredWeightGrams < NOISE_FLOOR_GRAMS) filteredWeightGrams = 0;

  return round(filteredWeightGrams * 10.0) / 10.0;
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
    }
    return;
  }

  if (liveWeightGrams < OBJECT_DETECT_GRAMS) {
    objectPresent = false;
    currentWeightGrams = 0.0;
    objectDetectCount = 0;
    objectRemoveCount = 0;
    lockMatchCount = 0;
    lastLockCandidateGrams = 0.0;
    return;
  }

  if (objectDetectCount < OBJECT_CONFIRM_SAMPLES) {
    objectDetectCount++;
    currentWeightGrams = 0.0;
    lockMatchCount = 1;
    lastLockCandidateGrams = liveWeightGrams;
    return;
  }

  objectPresent = true;
  objectRemoveCount = 0;
  currentWeightGrams = liveWeightGrams;
  if (liveWeightGrams == lastLockCandidateGrams) {
    if (lockMatchCount < LOCK_MATCH_SAMPLES) lockMatchCount++;
  } else {
    lastLockCandidateGrams = liveWeightGrams;
    lockMatchCount = 1;
  }

  if (lockMatchCount >= LOCK_MATCH_SAMPLES) {
    lockedWeightGrams = liveWeightGrams;
    currentWeightGrams = lockedWeightGrams;
    weightLocked = true;
    Serial.print("Weight locked(g): ");
    Serial.println(lockedWeightGrams, 1);
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
  String body = "{";
  body += "\"id\":";
  body += String(sale.id);
  body += ",\"weightGrams\":";
  body += String(sale.weightGrams, 2);
  body += ",\"weight\":";
  body += String(sale.weightGrams, 2);
  body += ",\"weightKg\":";
  body += String(sale.weightGrams / 1000.0, 3);
  body += ",\"price\":";
  body += String(sale.price, 2);
  body += ",\"pricePerKg\":";
  body += String(pricePerKg, 2);
  body += ",\"fruitType\":\"";
  body += jsonEscape(sale.fruitType);
  body += "\",\"fruit\":\"";
  body += jsonEscape(sale.fruitType);
  body += "\",\"fruit_type\":\"";
  body += jsonEscape(sale.fruitType);
  body += "\",\"timestamp\":\"";
  body += jsonEscape(sale.timestamp);
  body += "\",\"date\":\"";
  body += jsonEscape(sale.date);
  body += "\",\"time\":\"";
  body += jsonEscape(sale.time);
  body += "\",\"source\":\"";
  body += jsonEscape(sale.source);
  body += "\"}";
  return body;
}

SaleRecord recordSale(const char* source) {
  SaleRecord sale = {};
  sale.id = nextSaleId++;
  sale.weightGrams = currentWeightGrams;
  sale.price = calculatePrice(currentWeightGrams);
  strlcpy(sale.fruitType, currentFruitType, sizeof(sale.fruitType));
  strlcpy(sale.source, source, sizeof(sale.source));
  copyCurrentDateTime(sale.timestamp, sizeof(sale.timestamp),
                      sale.date, sizeof(sale.date),
                      sale.time, sizeof(sale.time));

  saleHistory[latestSaleId % SALE_HISTORY_SIZE] = sale;
  latestSaleId = sale.id;
  if (saleHistoryCount < SALE_HISTORY_SIZE) {
    saleHistoryCount++;
  }

  return sale;
}

void handleStatusRequest() {
  String body = "{";
  body += "\"ready\":";
  body += hx711Ready ? "true" : "false";
  body += ",\"weightGrams\":";
  body += String(currentWeightGrams, 2);
  body += ",\"price\":";
  body += String(calculatePrice(currentWeightGrams), 2);
  body += ",\"pricePerKg\":";
  body += String(pricePerKg, 2);
  body += ",\"status\":\"";
  body += scaleStatusText();
  body += "\",\"fruitType\":\"";
  body += jsonEscape(currentFruitType);
  body += "\",\"latestSaleId\":";
  body += String(latestSaleId);
  body += ",\"rtcReady\":";
  body += rtcReady ? "true" : "false";
  body += ",\"timestamp\":\"";
  body += currentTimestamp();
  body += "\",\"date\":\"";
  char date[11];
  char time[9];
  char timestamp[25];
  copyCurrentDateTime(timestamp, sizeof(timestamp), date, sizeof(date), time, sizeof(time));
  body += jsonEscape(date);
  body += "\",\"time\":\"";
  body += jsonEscape(time);
  body += "\",\"ip\":\"";
  body += WiFi.localIP().toString();
  body += "\"}";

  sendJson(200, body);
}

void handleTareRequest() {
  tareScale("App tare");
  handleStatusRequest();
}

void handleConfirmRequest() {
  SaleRecord sale = confirmSale("APP SALE OK", "app");
  sendJson(200, saleRecordJson(sale));
}

void handleLatestSaleRequest() {
  if (latestSaleId == 0) {
    sendJson(404, "{\"error\":\"No confirmed sale yet\"}");
    return;
  }

  for (size_t i = 0; i < saleHistoryCount; i++) {
    if (saleHistory[i].id == latestSaleId) {
      sendJson(200, saleRecordJson(saleHistory[i]));
      return;
    }
  }

  sendJson(404, "{\"error\":\"Latest sale not available\"}");
}

void handleSalesRequest() {
  String body = "[";
  bool first = true;
  for (size_t i = 0; i < saleHistoryCount; i++) {
    if (saleHistory[i].id == 0) continue;
    if (!first) body += ",";
    body += saleRecordJson(saleHistory[i]);
    first = false;
  }
  body += "]";
  sendJson(200, body);
}

void handleFruitRequest() {
  String fruit = server.arg("type");
  if (fruit.length() == 0 && server.hasArg("plain")) {
    fruit = server.arg("plain");
  }
  fruit.trim();

  if (fruit.length() == 0) {
    sendJson(400, "{\"error\":\"Missing fruit type\"}");
    return;
  }

  fruit.toCharArray(currentFruitType, sizeof(currentFruitType));
  handleStatusRequest();
}

void handleCancelRequest() {
  cancelSale("App cancel tare");
  handleStatusRequest();
}

void setupApiServer() {
  server.on("/", HTTP_GET, handleStatusRequest);
  server.on("/status", HTTP_GET, handleStatusRequest);
  server.on("/tare", HTTP_POST, handleTareRequest);
  server.on("/confirm", HTTP_POST, handleConfirmRequest);
  server.on("/cancel", HTTP_POST, handleCancelRequest);
  server.on("/fruit", HTTP_POST, handleFruitRequest);
  server.on("/sale/latest", HTTP_GET, handleLatestSaleRequest);
  server.on("/sales", HTTP_GET, handleSalesRequest);
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      handleOptions();
      return;
    }
    sendJson(404, "{\"error\":\"Not found\"}");
  });
  server.begin();

  Serial.print("Scale API: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/status");
}

void updateDisplay() {
  float billableWeightGrams = currentWeightGrams;
  if (billableWeightGrams < 0) billableWeightGrams = 0;
  float price = calculatePrice(billableWeightGrams);

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

  printScaleHelp();
}

void setup() {
  Serial.begin(57600);
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
  syncRtcFromNtp();
  setupScale();
  setupApiServer();

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
  server.handleClient();

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
