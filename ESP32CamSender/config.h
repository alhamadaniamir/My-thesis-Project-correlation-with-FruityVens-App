#pragma once
#include <stdint.h>

constexpr char kDeviceName[] = "constexprESP32-CAM (Gemini bridge)";
constexpr char kWifiSsid[] = "Aida_iPhone";
constexpr char kWifiPassword[] = "1234567899";

constexpr char kBackendUrl[] = "https://backend-kappa-roan-83.vercel.app/api/identify";
constexpr char kBackendAuthToken[] = "GSDDk++ltTHQk2lhvQfERYIdrdXFSmb7eDPalal0zqg=";
constexpr uint32_t kBackendHttpTimeoutMs = 15000;

constexpr int kLedPin = 13;
constexpr uint32_t kPlacementSettleMs = 1200;
constexpr uint32_t kDetectionTimeoutMs = 20000;
constexpr float kRoiX = 0.10f;
constexpr float kRoiY = 0.00f;
constexpr float kRoiW = 0.90f;
constexpr float kRoiH = 0.87f;
constexpr uint8_t kSnapshotJpegQuality = 80;
constexpr uint8_t kCaptureJpegQuality = 14;
constexpr uint32_t kPreviewIdleTimeoutMs = 20000;
