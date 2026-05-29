#pragma once
#include <esp_camera.h>
#include <stdint.h>

struct RoiRect {
  int x;
  int y;
  int w;
  int h;
};

bool initCamera();

RoiRect computeRoiRect(int src_w, int src_h);

// Draw a 3px red border around the ROI on an RGB565 frame (in place).
void drawRoiBox(camera_fb_t* fb);

// Capture one JPEG of the cropped ROI. Caller frees *out_buf with free().
bool captureRoiJpeg(uint8_t** out_buf, size_t* out_len, uint8_t jpeg_quality);

// Capture one JPEG of the full or cropped frame. Caller frees *out_buf.
bool captureFrameJpeg(uint8_t** out_buf, size_t* out_len, uint8_t jpeg_quality, bool draw_box, bool crop_roi);
