#pragma once
#include <stdint.h>

constexpr uint8_t kPacketTypeScaleCommand = 1;
constexpr uint8_t kPacketTypeDetectionResult = 2;

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
