#include <HX711_ADC.h>

#define HX_DOUT 23
#define HX_SCK 22

HX711_ADC LoadCell(HX_DOUT, HX_SCK);

unsigned long lastPrintMs = 0;

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("HX711 debug only");
  Serial.println("DT/DOUT -> GPIO 23");
  Serial.println("SCK     -> GPIO 22");
  Serial.println("Remove all weight while starting.");

  pinMode(HX_DOUT, INPUT);
  pinMode(HX_SCK, OUTPUT);

  LoadCell.begin();
  LoadCell.setSamplesInUse(1);
  LoadCell.start(2000, true);

  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
    Serial.println("ERROR: HX711 did not respond.");
    Serial.println("Check DT/DOUT, SCK, VCC, GND, and shared ESP32/HX711 ground.");
    while (true) {
      Serial.print("DOUT pin level: ");
      Serial.println(digitalRead(HX_DOUT));
      delay(1000);
    }
  }

  LoadCell.setCalFactor(1.0);
  Serial.println("HX711 responding.");
  Serial.println("Raw-ish value should change when you press/add weight.");
}

void loop() {
  if (LoadCell.update() && millis() - lastPrintMs >= 500) {
    Serial.print("value=");
    Serial.print(LoadCell.getData(), 0);
    Serial.print(" dout=");
    Serial.println(digitalRead(HX_DOUT));
    lastPrintMs = millis();
  }
}
