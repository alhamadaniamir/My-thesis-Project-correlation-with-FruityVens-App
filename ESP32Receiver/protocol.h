#pragma once
#include <stdint.h>

constexpr uint8_t kPacketTypeScaleCommand = 1;
constexpr uint8_t kPacketTypeDetectionResult = 2;
constexpr uint8_t kPacketTypeSaleSync = 3;
constexpr uint8_t kPacketTypePriceUpdate = 4;
constexpr uint8_t kPacketTypeSaleAck = 5;

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

struct SaleSyncPacket {
  uint8_t packetType;
  uint32_t sequence;
  uint32_t saleId;
  uint32_t createdAtMs;
  float weightGrams;
  float price;
  float pricePerKg;
  char fruitType[32];
  char timestamp[25];
  char date[11];
  char time[9];
  char source[16];
  char firebaseKey[80];
};

struct PriceUpdatePacket {
  uint8_t packetType;
  uint32_t sequence;
  float pricePerKg;
  char fruitType[32];
};

struct SaleAckPacket {
  uint8_t packetType;
  uint32_t sequence;
  bool accepted;
  char firebaseKey[80];
};
