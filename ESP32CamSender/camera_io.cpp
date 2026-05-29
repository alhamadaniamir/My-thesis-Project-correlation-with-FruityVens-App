#include "camera_io.h"
#include "config.h"

#include <Arduino.h>
#include "img_converters.h"

static camera_config_t camera_config = {};

bool initCamera() {
  camera_config.ledc_channel = LEDC_CHANNEL_0;
  camera_config.ledc_timer = LEDC_TIMER_0;
  camera_config.pin_d0 = 5;  camera_config.pin_d1 = 18;
  camera_config.pin_d2 = 19; camera_config.pin_d3 = 21;
  camera_config.pin_d4 = 36; camera_config.pin_d5 = 39;
  camera_config.pin_d6 = 34; camera_config.pin_d7 = 35;
  camera_config.pin_xclk = 0; camera_config.pin_pclk = 22;
  camera_config.pin_vsync = 25; camera_config.pin_href = 23;
  camera_config.pin_sccb_sda = 26; camera_config.pin_sccb_scl = 27;
  camera_config.pin_pwdn = 32; camera_config.pin_reset = -1;
  camera_config.xclk_freq_hz = 20000000;
  camera_config.pixel_format = PIXFORMAT_RGB565;
  camera_config.frame_size = psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  camera_config.jpeg_quality = 12;
  camera_config.fb_count = psramFound() ? 2 : 1;
  camera_config.grab_mode = CAMERA_GRAB_LATEST;
  if (psramFound()) camera_config.fb_location = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK) {
    Serial.printf("esp_camera_init() failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, 1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_lenc(s, 1);
  }
  return true;
}

RoiRect computeRoiRect(int src_w, int src_h) {
  RoiRect roi = {};
  roi.w = max(1, min(src_w, static_cast<int>(src_w * kRoiW)));
  roi.h = max(1, min(src_h, static_cast<int>(src_h * kRoiH)));
  const int max_x_offset = max(0, src_w - roi.w);
  const int max_y_offset = max(0, src_h - roi.h);
  roi.x = max(0, min(max_x_offset, static_cast<int>(src_w * kRoiX)));
  roi.y = max(0, min(max_y_offset, static_cast<int>(src_h * kRoiY)));
  return roi;
}

static void setRgb565Pixel(camera_fb_t* fb, int x, int y, uint16_t color) {
  if (x < 0 || y < 0 || x >= fb->width || y >= fb->height) return;
  const int index = (y * fb->width + x) * 2;
  fb->buf[index] = color & 0xFF;
  fb->buf[index + 1] = color >> 8;
}

void drawRoiBox(camera_fb_t* fb) {
  if (fb == nullptr || fb->format != PIXFORMAT_RGB565) return;
  const RoiRect roi = computeRoiRect(fb->width, fb->height);
  const uint16_t red = 0xF800;
  const int x1 = roi.x;
  const int y1 = roi.y;
  const int x2 = roi.x + roi.w - 1;
  const int y2 = roi.y + roi.h - 1;
  for (int t = 0; t < 3; ++t) {
    for (int x = x1; x <= x2; ++x) {
      setRgb565Pixel(fb, x, y1 + t, red);
      setRgb565Pixel(fb, x, y2 - t, red);
    }
    for (int y = y1; y <= y2; ++y) {
      setRgb565Pixel(fb, x1 + t, y, red);
      setRgb565Pixel(fb, x2 - t, y, red);
    }
  }
}

bool captureRoiJpeg(uint8_t** out_buf, size_t* out_len, uint8_t jpeg_quality) {
  return captureFrameJpeg(out_buf, out_len, jpeg_quality, /*draw_box=*/false, /*crop_roi=*/true);
}

bool captureFrameJpeg(uint8_t** out_buf, size_t* out_len, uint8_t jpeg_quality, bool draw_box, bool crop_roi) {
  *out_buf = nullptr;
  *out_len = 0;

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    Serial.println("Camera capture failed");
    return false;
  }
  if (draw_box) drawRoiBox(fb);

  camera_fb_t cropped_fb = *fb;
  camera_fb_t* frame = fb;
  uint8_t* cropped_buf = nullptr;
  if (crop_roi) {
    const RoiRect roi = computeRoiRect(fb->width, fb->height);
    const size_t len = static_cast<size_t>(roi.w) * roi.h * 2;
    cropped_buf = static_cast<uint8_t*>(psramFound() ? ps_malloc(len) : malloc(len));
    if (cropped_buf == nullptr) {
      esp_camera_fb_return(fb);
      Serial.println("ROI crop alloc failed");
      return false;
    }
    for (int y = 0; y < roi.h; ++y) {
      const size_t src = (static_cast<size_t>(roi.y + y) * fb->width + roi.x) * 2;
      const size_t dst = static_cast<size_t>(y) * roi.w * 2;
      memcpy(cropped_buf + dst, fb->buf + src, roi.w * 2);
    }
    cropped_fb.buf = cropped_buf;
    cropped_fb.len = len;
    cropped_fb.width = roi.w;
    cropped_fb.height = roi.h;
    cropped_fb.format = PIXFORMAT_RGB565;
    frame = &cropped_fb;
  }

  uint8_t* jpg = nullptr;
  size_t jpg_len = 0;
  const bool ok = frame2jpg(frame, jpeg_quality, &jpg, &jpg_len);
  if (cropped_buf) free(cropped_buf);
  esp_camera_fb_return(fb);
  if (!ok || !jpg || jpg_len == 0) {
    if (jpg) free(jpg);
    Serial.println("JPEG encode failed");
    return false;
  }

  *out_buf = jpg;
  *out_len = jpg_len;
  return true;
}
