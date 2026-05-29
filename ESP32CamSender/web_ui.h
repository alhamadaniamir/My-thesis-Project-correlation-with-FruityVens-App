#pragma once
#include <Arduino.h>

// Status the web UI reports.
struct CamUiStatus {
  const char* label;
  const char* monitor;
  float confidence;
  uint32_t sequence;
  uint32_t uptime_ms;
  bool active;
  bool preview;
  bool camera_on;
  const char* ip;
};

// Hooks the web UI calls back into the main sketch.
void onWebPreviewStart();
void onWebPreviewStop();
void onWebDetectStart();
void onWebDetectStop();
void onWebSnapshotRequested();  // Should call captureFrameJpeg + send via the request handler.
CamUiStatus getCamUiStatus();

void startWebServer();
void handleWebClient();
