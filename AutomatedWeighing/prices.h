#pragma once
#include "protocol.h"

struct FruitPrice {
  const char* fruitType;
  float pricePerKg;
};

// Collapse free-form labels into the canonical price-table keys.
const char* canonicalFruitType(const char* fruitType);

bool hasFruitPrice(const char* fruitType);
float pricePerKgForFruit(const char* fruitType);

// Returns true if the canonical fruit was found and its price was updated.
bool setFruitPrice(const char* fruitType, float pricePerKg);

// Apply a price update received over ESP-NOW.
void applyPriceUpdatePacket(const PriceUpdatePacket& packet);
