#include "prices.h"
#include <Arduino.h>
#include <string.h>

static FruitPrice fruitPrices[] = {
  {"Apple", 90.0f},        {"Orange", 85.0f},     {"Banana", 35.0f},
  {"Mango", 60.0f},        {"Grapes", 130.0f},    {"Lemon", 70.0f},
  {"Papaya", 50.0f},       {"Watermelon", 50.0f}, {"Pineapple", 45.0f},
  {"Calamansi", 65.0f},    {"Pomelo", 95.0f},     {"Guava", 50.0f},
  {"Avocado", 110.0f},     {"Coconut", 35.0f},    {"Dalandan", 75.0f},
  {"Dragon Fruit", 140.0f},{"Durian", 180.0f},    {"Mangosteen", 160.0f},
  {"Rambutan", 120.0f},    {"Lanzones", 120.0f},  {"Chico", 80.0f},
  {"Atis", 95.0f},         {"Santol", 70.0f},     {"Star Apple", 90.0f},
  {"Jackfruit", 65.0f},    {"Tamarind", 75.0f},   {"Melon", 55.0f},
  {"Guyabano", 100.0f},    {"Mango Carabao", 80.0f}, {"Indian Mango", 75.0f},
  {"Apple Mango", 60.0f},  {"Langkatan", 45.0f},  {"Pear", 95.0f},
  {"Strawberries", 120.0f},
};

const char* canonicalFruitType(const char* fruitType) {
  if (fruitType == nullptr || strlen(fruitType) == 0) return "";
  if (strcmp(fruitType, "Grape") == 0 ||
      strcmp(fruitType, "Grape Blue") == 0 ||
      strcmp(fruitType, "Grape Pink") == 0 ||
      strcmp(fruitType, "Grape White") == 0) return "Grapes";
  if (strcmp(fruitType, "Strawberry") == 0) return "Strawberries";
  if (strcmp(fruitType, "Carabao Mango") == 0) return "Mango Carabao";
  if (strcmp(fruitType, "Mango Red") == 0) return "Mango";
  if (strcmp(fruitType, "Lime") == 0 || strcmp(fruitType, "Limes") == 0) return "Lemon";
  if (strcmp(fruitType, "Mandarine") == 0) return "Orange";
  if (strcmp(fruitType, "Pomelo Sweetie") == 0) return "Pomelo";
  return fruitType;
}

bool hasFruitPrice(const char* fruitType) {
  const char* c = canonicalFruitType(fruitType);
  if (strlen(c) == 0) return false;
  for (size_t i = 0; i < sizeof(fruitPrices) / sizeof(fruitPrices[0]); i++) {
    if (strcmp(fruitPrices[i].fruitType, c) == 0 && fruitPrices[i].pricePerKg > 0.0f) return true;
  }
  return false;
}

float pricePerKgForFruit(const char* fruitType) {
  const char* c = canonicalFruitType(fruitType);
  if (strlen(c) == 0) return 0.0f;
  for (size_t i = 0; i < sizeof(fruitPrices) / sizeof(fruitPrices[0]); i++) {
    if (strcmp(fruitPrices[i].fruitType, c) == 0) return fruitPrices[i].pricePerKg;
  }
  return 0.0f;
}

bool setFruitPrice(const char* fruitType, float pricePerKg) {
  const char* c = canonicalFruitType(fruitType);
  if (strlen(c) == 0 || pricePerKg <= 0.0f) return false;
  for (size_t i = 0; i < sizeof(fruitPrices) / sizeof(fruitPrices[0]); i++) {
    if (strcmp(fruitPrices[i].fruitType, c) == 0) {
      fruitPrices[i].pricePerKg = pricePerKg;
      Serial.print("Price updated: ");
      Serial.print(c);
      if (strcmp(c, fruitType) != 0) { Serial.print(" from "); Serial.print(fruitType); }
      Serial.print(" = PHP ");
      Serial.print(pricePerKg, 2);
      Serial.println("/kg");
      return true;
    }
  }
  Serial.print("Ignored price update for unmanaged fruit: ");
  Serial.println(c);
  return false;
}

void applyPriceUpdatePacket(const PriceUpdatePacket& packet) {
  PriceUpdatePacket clean = packet;
  clean.fruitType[sizeof(clean.fruitType) - 1] = '\0';
  if (setFruitPrice(clean.fruitType, clean.pricePerKg)) {
    Serial.print("Worker price sync: ");
    Serial.print(clean.fruitType);
    Serial.print(" = PHP ");
    Serial.print(clean.pricePerKg, 2);
    Serial.println("/kg");
  }
}
