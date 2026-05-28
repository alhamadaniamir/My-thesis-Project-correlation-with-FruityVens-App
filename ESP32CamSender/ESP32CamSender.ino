#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_camera.h>
#include "img_converters.h"

#include "model_data.h"

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

constexpr char kDeviceName[] = "ESP32-CAM";
constexpr char kWifiSsid[] = "DITO_3CFF6_2.4";
constexpr char kWifiPassword[] = "48b84252";
constexpr int kLedPin = 13;
constexpr int kInputSize = 96;
constexpr size_t kTensorArenaSize = 1024 * 1024;
constexpr float kDetectionThreshold = 0.0f;
constexpr uint32_t kPlacementSettleMs = 1200;
constexpr uint32_t kDetectionIntervalMs = 450;
constexpr uint32_t kDetectionTimeoutMs = 12000;
constexpr uint8_t kDetectionSampleFrames = 10;
constexpr uint8_t kRequiredMatchingFrames = 5;
constexpr uint8_t kFallbackMatchingFrames = 2;
constexpr float kRoiX = 0.14f;
constexpr float kRoiY = 0.07f;
constexpr float kRoiW = 0.82f;
constexpr float kRoiH = 0.80f;
constexpr uint32_t kPreviewIdleTimeoutMs = 20000;
constexpr uint8_t kSnapshotJpegQuality = 80;

constexpr uint8_t kPacketTypeScaleCommand = 1;
constexpr uint8_t kPacketTypeDetectionResult = 2;

uint8_t kBroadcastPeer[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
IPAddress kStaticIp(192, 168, 1, 34);
IPAddress kGatewayIp(192, 168, 1, 1);
IPAddress kSubnetMask(255, 255, 255, 0);
IPAddress kDnsIp(8, 8, 8, 8);

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

struct DetectionVote {
  char label[32];
  uint8_t count;
  float confidence_sum;
  float best_confidence;
};

struct RoiRect {
  int x;
  int y;
  int w;
  int h;
};

struct LabelMapEntry {
  const char* raw_label;
  const char* mapped_label;
};

const LabelMapEntry kLabelMap[] = {
  {"Almonds 1", "Almonds 1"}, {"Apple 10", "Apple 10"}, {"Apple 11", "Apple 11"},
  {"Apple 12", "Apple 12"}, {"Apple 13", "Apple 13"}, {"Apple 14", "Apple 14"},
  {"Apple 17", "Apple 17"}, {"Apple 18", "Apple 18"}, {"Apple 19", "Apple 19"},
  {"Apple 20", "Apple 20"}, {"Apple 21", "Apple 21"}, {"Apple 22", "Apple 22"},
  {"Apple 23", "Apple 23"}, {"Apple 5", "Apple 5"}, {"Apple 6", "Apple 6"},
  {"Apple 7", "Apple 7"}, {"Apple 8", "Apple 8"}, {"Apple 9", "Apple 9"},
  {"Apple Braeburn 1", "Apple Braeburn 1"}, {"Apple Crimson Snow 1", "Apple Crimson Snow 1"},
  {"Apple Golden 1", "Apple Golden 1"}, {"Apple Golden 2", "Apple Golden 2"},
  {"Apple Golden 3", "Apple Golden 3"}, {"Apple Granny Smith 1", "Apple Granny Smith 1"},
  {"Apple Pink Lady 1", "Apple Pink Lady 1"}, {"Apple Red 1", "Apple Red 1"},
  {"Apple Red 2", "Apple Red 2"}, {"Apple Red 3", "Apple Red 3"},
  {"Apple Red Delicious 1", "Apple Red Delicious 1"}, {"Apple Red Yellow 1", "Apple Red Yellow 1"},
  {"Apple Red Yellow 2", "Apple Red Yellow 2"}, {"Apricot 1", "Apricot 1"},
  {"Avocado 1", "Avocado 1"}, {"Avocado 2", "Avocado 2"},
  {"Avocado Black 1", "Avocado Black 1"}, {"Avocado Black 2", "Avocado Black 2"},
  {"Avocado Green 1", "Avocado Green 1"}, {"Banana 1", "Banana 1"},
  {"Banana 3", "Banana 3"}, {"Banana 4", "Banana 4"},
  {"Banana Lady Finger 1", "Banana Lady Finger 1"}, {"Banana Red 1", "Banana Red 1"},
  {"Bean pod 1", "Bean pod 1"}, {"Beetroot 1", "Beetroot 1"},
  {"BlackBerry 4", "BlackBerry 4"}, {"Blackberry 1", "Blackberry 1"},
  {"Blackberry 2", "Blackberry 2"}, {"Blackberry 3", "Blackberry 3"},
  {"Blackberry 5", "Blackberry 5"}, {"Blueberry 1", "Blueberry 1"},
  {"Cabbage red 1", "Cabbage red 1"}, {"Cabbage white 1", "Cabbage white 1"},
  {"Cactus fruit 1", "Cactus fruit 1"}, {"Cactus fruit green 1", "Cactus fruit green 1"},
  {"Cactus fruit red 1", "Cactus fruit red 1"}, {"Caju seed 1", "Caju seed 1"},
  {"Cantaloupe 1", "Cantaloupe 1"}, {"Cantaloupe 2", "Cantaloupe 2"},
  {"Cantaloupe 3", "Cantaloupe 3"}, {"Carambola 1", "Carambola 1"},
  {"Carambola 2", "Carambola 2"}, {"Carrot 1", "Carrot 1"},
  {"Cauliflower 1", "Cauliflower 1"}, {"Cherimoya 1", "Cherimoya 1"},
  {"Cherry 1", "Cherry 1"}, {"Cherry 2", "Cherry 2"}, {"Cherry 3", "Cherry 3"},
  {"Cherry 4", "Cherry 4"}, {"Cherry 5", "Cherry 5"},
  {"Cherry Rainier 1", "Cherry Rainier 1"}, {"Cherry Rainier 2", "Cherry Rainier 2"},
  {"Cherry Rainier 3", "Cherry Rainier 3"}, {"Cherry Sour 1", "Cherry Sour 1"},
  {"Cherry Wax 1", "Cherry Wax 1"}, {"Cherry Wax 2", "Cherry Wax 2"},
  {"Cherry Wax Black 1", "Cherry Wax Black 1"}, {"Cherry Wax Red 1", "Cherry Wax Red 1"},
  {"Cherry Wax Red 2", "Cherry Wax Red 2"}, {"Cherry Wax Red 3", "Cherry Wax Red 3"},
  {"Cherry Wax Yellow 1", "Cherry Wax Yellow 1"}, {"Chestnut 1", "Chestnut 1"},
  {"Clementine 1", "Clementine 1"}, {"Cocos 1", "Cocos 1"}, {"Corn 1", "Corn 1"},
  {"Corn Husk 1", "Corn Husk 1"}, {"Cucumber 1", "Cucumber 1"},
  {"Cucumber 10", "Cucumber 10"}, {"Cucumber 11", "Cucumber 11"},
  {"Cucumber 12", "Cucumber 12"}, {"Cucumber 13", "Cucumber 13"},
  {"Cucumber 3", "Cucumber 3"}, {"Cucumber 4", "Cucumber 4"},
  {"Cucumber 5", "Cucumber 5"}, {"Cucumber 6", "Cucumber 6"},
  {"Cucumber 7", "Cucumber 7"}, {"Cucumber 8", "Cucumber 8"},
  {"Cucumber 9", "Cucumber 9"}, {"Dates 1", "Dates 1"}, {"Dates 2", "Dates 2"},
  {"Eggplant 1", "Eggplant 1"}, {"Eggplant long 1", "Eggplant long 1"},
  {"Fig 1", "Fig 1"}, {"Ginger 2", "Ginger 2"}, {"Ginger Root 1", "Ginger Root 1"},
  {"Gooseberry 1", "Gooseberry 1"}, {"Granadilla 1", "Granadilla 1"},
  {"Grape 1", "Grape 1"}, {"Grape Blue 1", "Grape Blue 1"},
  {"Grape Pink 1", "Grape Pink 1"}, {"Grape White 1", "Grape White 1"},
  {"Grape White 2", "Grape White 2"}, {"Grape White 3", "Grape White 3"},
  {"Grape White 4", "Grape White 4"}, {"Grape pink 2", "Grape pink 2"},
  {"Grapefruit Pink 1", "Grapefruit Pink 1"}, {"Grapefruit White 1", "Grapefruit White 1"},
  {"Guava 1", "Guava 1"}, {"Hazelnut 1", "Hazelnut 1"},
  {"Huckleberry 1", "Huckleberry 1"}, {"Kaki 1", "Kaki 1"}, {"Kiwi 1", "Kiwi 1"},
  {"Kohlrabi 1", "Kohlrabi 1"}, {"Kumquats 1", "Kumquats 1"},
  {"Lemon 1", "Lemon 1"}, {"Lemon Meyer 1", "Lemon Meyer 1"},
  {"Limes 1", "Limes 1"}, {"Lychee 1", "Lychee 1"},
  {"Mandarine 1", "Mandarine 1"}, {"Mango 1", "Mango 1"},
  {"Mango Red 1", "Mango Red 1"}, {"Mangostan 1", "Mangostan 1"},
  {"Maracuja 1", "Maracuja 1"}, {"Melon Piel de Sapo 1", "Melon Piel de Sapo 1"},
  {"Mulberry 1", "Mulberry 1"}, {"Nectarine 1", "Nectarine 1"},
  {"Nectarine Flat 1", "Nectarine Flat 1"}, {"Nectarine Flat 2", "Nectarine Flat 2"},
  {"Nut 1", "Nut 1"}, {"Nut 2", "Nut 2"}, {"Nut 3", "Nut 3"}, {"Nut 4", "Nut 4"},
  {"Nut 5", "Nut 5"}, {"Nut Forest 1", "Nut Forest 1"},
  {"Nut Pecan 1", "Nut Pecan 1"}, {"Onion 2", "Onion 2"},
  {"Onion Red 1", "Onion Red 1"}, {"Onion Red 2", "Onion Red 2"},
  {"Onion Red 3", "Onion Red 3"}, {"Onion White 1", "Onion White 1"},
  {"Onion White 2", "Onion White 2"}, {"Orange 1", "Orange 1"},
  {"Orange 2", "Orange 2"}, {"Orange 3", "Orange 3"},
  {"Orange peeled 1", "Orange peeled 1"}, {"Papaya 1", "Papaya 1"},
  {"Papaya 2", "Papaya 2"}, {"Passion Fruit 1", "Passion Fruit 1"},
  {"Peach 1", "Peach 1"}, {"Peach 2", "Peach 2"}, {"Peach 3", "Peach 3"},
  {"Peach 4", "Peach 4"}, {"Peach 5", "Peach 5"}, {"Peach 6", "Peach 6"},
  {"Peach Flat 1", "Peach Flat 1"}, {"Peanut shell 1x 1", "Peanut shell 1x 1"},
  {"Pear 1", "Pear 1"}, {"Pear 10", "Pear 10"}, {"Pear 11", "Pear 11"},
  {"Pear 12", "Pear 12"}, {"Pear 13", "Pear 13"}, {"Pear 14", "Pear 14"},
  {"Pear 2", "Pear 2"}, {"Pear 3", "Pear 3"}, {"Pear 5", "Pear 5"},
  {"Pear 6", "Pear 6"}, {"Pear 7", "Pear 7"}, {"Pear 8", "Pear 8"},
  {"Pear 9", "Pear 9"}, {"Pear Abate 1", "Pear Abate 1"},
  {"Pear Forelle 1", "Pear Forelle 1"}, {"Pear Kaiser 1", "Pear Kaiser 1"},
  {"Pear Monster 1", "Pear Monster 1"}, {"Pear Red 1", "Pear Red 1"},
  {"Pear Stone 1", "Pear Stone 1"}, {"Pear Williams 1", "Pear Williams 1"},
  {"Pepino 1", "Pepino 1"}, {"Pepper 1", "Pepper 1"}, {"Pepper 2", "Pepper 2"},
  {"Pepper Green 1", "Pepper Green 1"}, {"Pepper Orange 1", "Pepper Orange 1"},
  {"Pepper Orange 2", "Pepper Orange 2"}, {"Pepper Red 1", "Pepper Red 1"},
  {"Pepper Red 2", "Pepper Red 2"}, {"Pepper Red 3", "Pepper Red 3"},
  {"Pepper Red 4", "Pepper Red 4"}, {"Pepper Red 5", "Pepper Red 5"},
  {"Pepper Yellow 1", "Pepper Yellow 1"}, {"Physalis 1", "Physalis 1"},
  {"Physalis with Husk 1", "Physalis with Husk 1"}, {"Pineapple 1", "Pineapple 1"},
  {"Pineapple Mini 1", "Pineapple Mini 1"}, {"Pistachio 1", "Pistachio 1"},
  {"Pitahaya Red 1", "Pitahaya Red 1"}, {"Plum 1", "Plum 1"}, {"Plum 2", "Plum 2"},
  {"Plum 3", "Plum 3"}, {"Plum 4", "Plum 4"}, {"Plum 5", "Plum 5"},
  {"Pomegranate 1", "Pomegranate 1"}, {"Pomelo Sweetie 1", "Pomelo Sweetie 1"},
  {"Potato Red 1", "Potato Red 1"}, {"Potato Red 2", "Potato Red 2"},
  {"Potato Sweet 1", "Potato Sweet 1"}, {"Potato White 1", "Potato White 1"},
  {"Quince 1", "Quince 1"}, {"Quince 2", "Quince 2"}, {"Quince 3", "Quince 3"},
  {"Quince 4", "Quince 4"}, {"Rambutan 1", "Rambutan 1"},
  {"Raspberry 1", "Raspberry 1"}, {"Raspberry 2", "Raspberry 2"},
  {"Raspberry 3", "Raspberry 3"}, {"Raspberry 4", "Raspberry 4"},
  {"Raspberry 5", "Raspberry 5"}, {"Raspberry 6", "Raspberry 6"},
  {"Redcurrant 1", "Redcurrant 1"}, {"Salak 1", "Salak 1"},
  {"Strawberry 1", "Strawberry 1"}, {"Strawberry 2", "Strawberry 2"},
  {"Strawberry 3", "Strawberry 3"}, {"Strawberry Wedge 1", "Strawberry Wedge 1"},
  {"Tamarillo 1", "Tamarillo 1"}, {"Tangelo 1", "Tangelo 1"},
  {"Tomato 1", "Tomato 1"}, {"Tomato 10", "Tomato 10"},
  {"Tomato 11", "Tomato 11"}, {"Tomato 2", "Tomato 2"}, {"Tomato 3", "Tomato 3"},
  {"Tomato 4", "Tomato 4"}, {"Tomato 5", "Tomato 5"}, {"Tomato 7", "Tomato 7"},
  {"Tomato 8", "Tomato 8"}, {"Tomato 9", "Tomato 9"},
  {"Tomato Cherry Maroon 1", "Tomato Cherry Maroon 1"},
  {"Tomato Cherry Orange 1", "Tomato Cherry Orange 1"},
  {"Tomato Cherry Red 1", "Tomato Cherry Red 1"},
  {"Tomato Cherry Red 2", "Tomato Cherry Red 2"},
  {"Tomato Cherry Yellow 1", "Tomato Cherry Yellow 1"},
  {"Tomato Heart 1", "Tomato Heart 1"}, {"Tomato Maroon 1", "Tomato Maroon 1"},
  {"Tomato Maroon 2", "Tomato Maroon 2"}, {"Tomato Yellow 1", "Tomato Yellow 1"},
  {"Walnut 1", "Walnut 1"}, {"Watermelon 1", "Watermelon 1"},
  {"Zucchini 1", "Zucchini 1"}, {"Zucchini Green 1", "Zucchini Green 1"},
  {"Zucchini dark 1", "Zucchini dark 1"}
};

camera_config_t camera_config = {};

const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input_tensor = nullptr;
TfLiteTensor* output_tensor = nullptr;
uint8_t* tensor_arena = nullptr;
uint32_t sequence_id = 0;
uint32_t command_sequence_id = 0;
uint32_t last_detection_ms = 0;
uint32_t detection_started_ms = 0;
uint32_t last_preview_ms = 0;
uint8_t detection_frames_seen = 0;
DetectionVote detection_votes[kDetectionSampleFrames] = {};
WebServer web_server(80);
char latest_label[32] = "Idle";
char latest_raw_label[32] = "";
char latest_normalized_label[32] = "";
char latest_candidate_label[32] = "";
char latest_monitor_line[160] = "Waiting for scale detection...";
float latest_confidence = 0.0f;
uint32_t latest_uptime_ms = 0;
uint8_t latest_frame_seen = 0;
bool latest_blocked = false;
bool detection_active = false;
bool detection_result_sent = false;
bool preview_active = false;
bool esp_now_ready = false;
bool camera_ready = false;
bool model_ready = false;

String macToString(const uint8_t* mac) {
  char buffer[18];
  snprintf(
    buffer,
    sizeof(buffer),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
  return String(buffer);
}

const char* mapLabel(const char* raw_label) {
  for (size_t i = 0; i < sizeof(kLabelMap) / sizeof(kLabelMap[0]); ++i) {
    if (strcmp(kLabelMap[i].raw_label, raw_label) == 0) {
      return kLabelMap[i].mapped_label;
    }
  }
  return raw_label;
}

bool labelStartsWith(const char* label, const char* prefix) {
  return strncmp(label, prefix, strlen(prefix)) == 0;
}

const char* normalizeFruitName(const char* label) {
  if (labelStartsWith(label, "Apple")) return "Apple";
  if (labelStartsWith(label, "Avocado")) return "Avocado";
  if (labelStartsWith(label, "Banana")) return "Banana";
  if (labelStartsWith(label, "Grape")) return "Grapes";
  if (labelStartsWith(label, "Guava")) return "Guava";
  if (labelStartsWith(label, "Lemon") || labelStartsWith(label, "Limes")) return "Lemon";
  if (labelStartsWith(label, "Mango")) return "Mango";
  if (labelStartsWith(label, "Orange") || labelStartsWith(label, "Mandarine")) return "Orange";
  if (labelStartsWith(label, "Papaya")) return "Papaya";
  if (labelStartsWith(label, "Pear")) return "Pear";
  if (labelStartsWith(label, "Pineapple")) return "Pineapple";
  if (labelStartsWith(label, "Pomelo")) return "Pomelo";
  if (labelStartsWith(label, "Rambutan")) return "Rambutan";
  if (labelStartsWith(label, "Strawberry")) return "Strawberries";
  if (labelStartsWith(label, "Watermelon")) return "Watermelon";
  return label;
}

bool isAllowedFruitName(const char* label) {
  return strcmp(label, "Apple") == 0 ||
         strcmp(label, "Avocado") == 0 ||
         strcmp(label, "Banana") == 0 ||
         strcmp(label, "Grapes") == 0 ||
         strcmp(label, "Guava") == 0 ||
         strcmp(label, "Lemon") == 0 ||
         strcmp(label, "Mango") == 0 ||
         strcmp(label, "Orange") == 0 ||
         strcmp(label, "Papaya") == 0 ||
         strcmp(label, "Pear") == 0 ||
         strcmp(label, "Pineapple") == 0 ||
         strcmp(label, "Pomelo") == 0 ||
         strcmp(label, "Rambutan") == 0 ||
         strcmp(label, "Strawberries") == 0 ||
         strcmp(label, "Watermelon") == 0;
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

void setRgb565Pixel(camera_fb_t* fb, int x, int y, uint16_t color) {
  if (x < 0 || y < 0 || x >= fb->width || y >= fb->height) {
    return;
  }

  const int index = (y * fb->width + x) * 2;
  fb->buf[index] = color & 0xFF;
  fb->buf[index + 1] = color >> 8;
}

void drawRoiBox(camera_fb_t* fb) {
  if (fb == nullptr || fb->format != PIXFORMAT_RGB565) {
    return;
  }

  const RoiRect roi = computeRoiRect(fb->width, fb->height);
  const uint16_t red = 0xF800;
  const int x1 = roi.x;
  const int y1 = roi.y;
  const int x2 = roi.x + roi.w - 1;
  const int y2 = roi.y + roi.h - 1;

  for (int thickness = 0; thickness < 3; ++thickness) {
    for (int x = x1; x <= x2; ++x) {
      setRgb565Pixel(fb, x, y1 + thickness, red);
      setRgb565Pixel(fb, x, y2 - thickness, red);
    }
    for (int y = y1; y <= y2; ++y) {
      setRgb565Pixel(fb, x1 + thickness, y, red);
      setRgb565Pixel(fb, x2 - thickness, y, red);
    }
  }
}

bool cameraOutputActive() {
  return detection_active || preview_active;
}

void resetLatestDetectionInfo(const char* monitor_line) {
  latest_raw_label[0] = '\0';
  latest_normalized_label[0] = '\0';
  latest_candidate_label[0] = '\0';
  latest_frame_seen = 0;
  latest_blocked = false;
  strlcpy(latest_monitor_line, monitor_line, sizeof(latest_monitor_line));
}

void appendJsonString(String& json, const char* value) {
  json += "\"";
  for (const char* cursor = value; *cursor != '\0'; ++cursor) {
    if (*cursor == '"' || *cursor == '\\') {
      json += '\\';
      json += *cursor;
    } else if (*cursor == '\n') {
      json += "\\n";
    } else if (*cursor == '\r') {
      json += "\\r";
    } else {
      json += *cursor;
    }
  }
  json += "\"";
}

void updateCameraOutput() {
  digitalWrite(kLedPin, cameraOutputActive() ? HIGH : LOW);
}

void resetDetectionVotes() {
  detection_frames_seen = 0;
  for (uint8_t i = 0; i < kDetectionSampleFrames; ++i) {
    detection_votes[i].label[0] = '\0';
    detection_votes[i].count = 0;
    detection_votes[i].confidence_sum = 0.0f;
    detection_votes[i].best_confidence = 0.0f;
  }
}

void setDetectionActive(bool active, const char* reason) {
  if (detection_active == active) {
    return;
  }

  detection_active = active;
  updateCameraOutput();
  if (active) {
    detection_started_ms = millis();
    last_detection_ms = 0;
    resetDetectionVotes();
    detection_result_sent = false;
    latest_confidence = 0.0f;
    latest_uptime_ms = millis();
    strlcpy(latest_label, "Settling", sizeof(latest_label));
    resetLatestDetectionInfo("Detection started: waiting for fruit to settle");
  } else {
    resetDetectionVotes();
    if (!detection_result_sent) {
      strlcpy(latest_label, "Idle", sizeof(latest_label));
      latest_confidence = 0.0f;
      latest_uptime_ms = millis();
      resetLatestDetectionInfo("Detection stopped");
    }
  }

  Serial.print(active ? "Detection ON: " : "Detection OFF: ");
  Serial.println(reason);
}

void setPreviewActive(bool active, const char* reason) {
  if (active) {
    last_preview_ms = millis();
  }

  if (preview_active == active) {
    return;
  }

  preview_active = active;
  updateCameraOutput();
  if (active && !detection_active && strcmp(latest_label, "Idle") == 0) {
    strlcpy(latest_label, "Preview", sizeof(latest_label));
    latest_confidence = 0.0f;
    latest_uptime_ms = millis();
    resetLatestDetectionInfo("Preview on: camera frame visible");
  } else if (!cameraOutputActive() && !detection_result_sent) {
    strlcpy(latest_label, "Idle", sizeof(latest_label));
    latest_confidence = 0.0f;
    latest_uptime_ms = millis();
    resetLatestDetectionInfo("Preview off");
  }

  Serial.print(active ? "Preview ON: " : "Preview OFF: ");
  Serial.println(reason);
}

void onSendComplete(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
  Serial.print("ESP-NOW send to ");
  if (tx_info != nullptr && tx_info->des_addr != nullptr) {
    Serial.print(macToString(tx_info->des_addr));
  } else {
    Serial.print("unknown");
  }
  Serial.print(" -> ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void onPacketReceived(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len) {
  if (len != sizeof(ScaleCommandPacket)) {
    return;
  }

  ScaleCommandPacket packet = {};
  memcpy(&packet, data, sizeof(packet));
  packet.command[sizeof(packet.command) - 1] = '\0';
  if (packet.packetType != kPacketTypeScaleCommand) {
    return;
  }

  command_sequence_id = packet.sequence;
  Serial.print("Scale command ");
  Serial.print(packet.command);
  Serial.print(" from ");
  Serial.println(macToString(recv_info->src_addr));

  if (strcmp(packet.command, "START") == 0) {
    setDetectionActive(true, "scale weight detected");
  } else if (strcmp(packet.command, "STOP") == 0) {
    setDetectionActive(false, "scale stopped");
  }
}

bool addBroadcastPeer() {
  if (esp_now_is_peer_exist(kBroadcastPeer)) {
    return true;
  }

  uint8_t channel = WiFi.channel();
  if (channel == 0) {
    channel = 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  }

  esp_now_peer_info_t peer_info = {};
  memcpy(peer_info.peer_addr, kBroadcastPeer, 6);
  peer_info.channel = channel;
  peer_info.encrypt = false;

  if (esp_now_add_peer(&peer_info) != ESP_OK) {
    Serial.println("esp_now_add_peer(broadcast) failed");
    return false;
  }

  return true;
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  if (!WiFi.config(kStaticIp, kGatewayIp, kSubnetMask, kDnsIp)) {
    Serial.println("Static IP config failed; DHCP will be used");
  }
  WiFi.begin(kWifiSsid, kWifiPassword);

  Serial.print("Connecting WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi FAILED");
    WiFi.disconnect(false, false);
    return false;
  }

  Serial.print("WiFi OK. IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("WiFi channel: ");
  Serial.println(WiFi.channel());
  return true;
}

bool initEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init() failed");
    return false;
  }

  esp_now_register_send_cb(onSendComplete);
  esp_now_register_recv_cb(onPacketReceived);

  if (!addBroadcastPeer()) {
    return false;
  }

  Serial.print("Sender MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Web address: http://");
  Serial.println(WiFi.localIP());
  Serial.println("ESP-NOW ready for scale START/STOP commands");
  return true;
}

bool sendCameraJpeg(bool draw_roi_box) {
  if (!camera_ready) {
    web_server.send(503, "text/plain", "Camera is not ready");
    return false;
  }

  last_preview_ms = millis();
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    web_server.send(503, "text/plain", "Camera capture failed");
    return false;
  }

  if (draw_roi_box) {
    drawRoiBox(fb);
  }

  uint8_t* jpg_buffer = nullptr;
  size_t jpg_length = 0;
  const bool converted = frame2jpg(
    fb,
    kSnapshotJpegQuality,
    &jpg_buffer,
    &jpg_length
  );
  esp_camera_fb_return(fb);

  if (!converted || jpg_buffer == nullptr || jpg_length == 0) {
    if (jpg_buffer != nullptr) {
      free(jpg_buffer);
    }
    web_server.send(500, "text/plain", "JPEG conversion failed");
    return false;
  }

  web_server.sendHeader("Cache-Control", "no-store");
  web_server.setContentLength(jpg_length);
  web_server.send(200, "image/jpeg", "");
  web_server.client().write(jpg_buffer, jpg_length);
  free(jpg_buffer);
  return true;
}

void handleSnapshotRequest() {
  if (!cameraOutputActive()) {
    web_server.send(409, "text/plain", "Camera is idle");
    return;
  }

  sendCameraJpeg(true);
}

void handleDatasetImageRequest() {
  if (!preview_active) {
    setPreviewActive(true, "dataset capture");
  }

  sendCameraJpeg(false);
}

void handleDatasetPageRequest() {
  const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>FruitCam Dataset Capture</title>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    body{font-family:Arial,sans-serif;background:#101820;color:white;margin:0;padding:18px;text-align:center;}
    main{max-width:620px;margin:0 auto;}
    h2{margin:10px 0 14px;}
    img{width:100%;max-width:560px;background:#000;border:1px solid #3f5566;border-radius:8px;}
    button,select,input{font-size:16px;padding:10px 12px;margin:5px;border-radius:8px;border:0;}
    button{color:white;background:#1565c0;}
    button.stop{background:#b71c1c;}
    a{color:#8ee59b;text-decoration:none;}
    .panel{background:#1d2a35;border-radius:10px;padding:16px;}
    .row{display:flex;flex-wrap:wrap;justify-content:center;align-items:center;gap:6px;margin:8px 0;}
    .status{color:#b8c1cc;font-size:14px;line-height:1.4;min-height:20px;}
    .count{color:#8ee59b;font-weight:700;}
  </style>
</head>
<body>
  <main>
    <h2>Dataset Capture</h2>
    <div class="panel">
      <img id="preview" src="/dataset.jpg">
      <div class="row">
        <select id="label">
          <option>Apple</option>
          <option>Orange</option>
          <option>Banana</option>
          <option>Mango</option>
          <option>Pear</option>
          <option>Empty</option>
          <option>Hand</option>
          <option>PlasticBag</option>
          <option>Mixed</option>
          <option>Other</option>
        </select>
        <button onclick="captureOne()">Capture One</button>
      </div>
      <div class="row">
        <input id="burstCount" type="number" min="1" max="100" value="20">
        <input id="burstDelay" type="number" min="300" max="5000" value="900">
        <button onclick="startBurst()">Start Burst</button>
        <button class="stop" onclick="stopBurst()">Stop</button>
      </div>
      <p class="status" id="status">Saved in browser downloads. Sort files into folders by label.</p>
      <p class="status">Captured this page: <span class="count" id="captured">0</span></p>
      <p class="status"><a href="/">Back to live preview</a></p>
    </div>
  </main>
  <script>
    let captured=0;
    let burstTimer=null;
    let burstLeft=0;
    let busy=false;

    function cleanLabel(){
      return document.getElementById('label').value.replace(/[^A-Za-z0-9_-]/g,'');
    }

    function stamp(){
      return new Date().toISOString().replace(/[-:.TZ]/g,'').slice(0,14);
    }

    function setStatus(text){
      document.getElementById('status').textContent=text;
    }

    async function captureOne(){
      if(busy){return;}
      busy=true;
      try{
        const label=cleanLabel();
        const r=await fetch('/dataset.jpg?ts='+Date.now());
        if(!r.ok){throw new Error(await r.text());}
        const blob=await r.blob();
        const url=URL.createObjectURL(blob);
        const a=document.createElement('a');
        a.href=url;
        a.download=label+'_'+stamp()+'_'+String(captured+1).padStart(4,'0')+'.jpg';
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(()=>URL.revokeObjectURL(url),1000);
        captured++;
        document.getElementById('captured').textContent=captured;
        setStatus('Captured '+a.download);
      }catch(e){
        setStatus('Capture failed: '+e.message);
      }
      busy=false;
    }

    function startBurst(){
      stopBurst();
      burstLeft=Math.max(1,Math.min(100,Number(document.getElementById('burstCount').value)||20));
      const delay=Math.max(300,Math.min(5000,Number(document.getElementById('burstDelay').value)||900));
      setStatus('Burst running: '+burstLeft+' images');
      burstTimer=setInterval(async()=>{
        if(burstLeft<=0){stopBurst();return;}
        await captureOne();
        burstLeft--;
        if(burstLeft<=0){stopBurst();}
      },delay);
    }

    function stopBurst(){
      if(burstTimer!==null){
        clearInterval(burstTimer);
        burstTimer=null;
        setStatus('Burst stopped');
      }
    }

    setInterval(()=>{
      document.getElementById('preview').src='/dataset.jpg?ts='+Date.now();
    },900);
  </script>
</body>
</html>
)rawliteral";

  web_server.send(200, "text/html", html);
}

void startWebServer() {
  web_server.on("/", []() {
    const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>FruitCam Live</title>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    body{font-family:Arial,sans-serif;background:#101820;color:white;margin:0;padding:18px;text-align:center;}
    main{max-width:560px;margin:0 auto;}
    h2{margin:10px 0 14px;}
    img{display:none;width:100%;max-width:520px;background:#000;border:1px solid #3f5566;border-radius:8px;}
    button{font-size:18px;padding:12px 18px;margin:6px;border:0;border-radius:8px;color:white;}
    .on{background:#2e7d32;}.off{background:#b71c1c;}.test{background:#1565c0;}
    .controls{margin-top:10px;}
    .panel{background:#1d2a35;border-radius:10px;padding:16px;}
    .fruit{font-size:34px;font-weight:700;margin:12px 0;word-break:break-word;}
    .confidence{font-size:18px;color:#8ee59b;margin-bottom:8px;}
    .meta,.idle{color:#b8c1cc;font-size:14px;line-height:1.4;}
    .monitor{margin-top:14px;background:#0b1117;border:1px solid #31475a;border-radius:8px;padding:12px;text-align:left;}
    .monitor-title{color:#8ee59b;font-size:13px;font-weight:700;margin-bottom:8px;text-transform:uppercase;}
    .monitor-line{color:#eef6ff;font-family:Consolas,Monaco,monospace;font-size:13px;line-height:1.45;min-height:38px;white-space:pre-wrap;word-break:break-word;margin:0 0 10px;}
    .monitor-grid{display:grid;grid-template-columns:92px 1fr;gap:5px 10px;color:#b8c1cc;font-size:13px;}
    .monitor-grid b{color:white;font-weight:600;}
  </style>
</head>
<body>
  <main>
    <h2>FruitCam Live</h2>
    <div class="panel">
      <img id="preview" src="/snapshot.jpg">
      <p class="idle" id="idle">Camera idle</p>
      <div class="fruit" id="label">Idle</div>
      <div class="confidence" id="confidence">0% confidence</div>
      <div class="meta" id="meta">Waiting for scale detection...</div>
      <div class="controls">
        <button class="on" onclick="cmd('/preview/start')">Preview ON</button>
        <button class="off" onclick="cmd('/preview/stop')">Preview OFF</button>
        <button class="test" onclick="cmd('/detect/start')">Detect Test</button>
        <button class="off" onclick="cmd('/detect/stop')">Stop</button>
      </div>
      <div class="monitor">
        <div class="monitor-title">Mini monitor</div>
        <pre class="monitor-line" id="monitorLine">Waiting for scale detection...</pre>
        <div class="monitor-grid">
          <span>Frames</span><b id="monitorFrames">0/10</b>
          <span>Raw</span><b id="monitorRaw">-</b>
          <span>Accepted</span><b id="monitorAccepted">-</b>
          <span>Blocked</span><b id="monitorBlocked">No</b>
        </div>
      </div>
    </div>
  </main>
  <script>
    async function cmd(path){
      try{await fetch(path+'?ts='+Date.now());}catch(e){}
      refresh();
    }
    async function refresh(){
      try{
        const r=await fetch('/status?ts='+Date.now());
        const s=await r.json();
        const img=document.getElementById('preview');
        const idle=document.getElementById('idle');
        const cameraOn=s.cameraOn;
        document.getElementById('label').textContent=s.label;
        document.getElementById('confidence').textContent=Math.round(s.confidence*100)+'% confidence';
        document.getElementById('meta').textContent=(s.active?'Detecting':'Idle')+' | Preview '+(s.preview?'ON':'OFF')+' | LED '+(s.led?'ON':'OFF')+' | IP '+s.ip;
        document.getElementById('monitorLine').textContent=s.monitor;
        document.getElementById('monitorFrames').textContent=s.frames_seen+'/'+s.sample_frames+' frames, need '+s.required_frames+', final accepts '+s.fallback_frames;
        document.getElementById('monitorRaw').textContent=s.raw || '-';
        document.getElementById('monitorAccepted').textContent=s.candidate || '-';
        document.getElementById('monitorBlocked').textContent=s.blocked?'Yes':'No';
        img.style.display=cameraOn?'block':'none';
        idle.style.display=cameraOn?'none':'block';
        if(cameraOn){img.src='/snapshot.jpg?ts='+Date.now();}
      }catch(e){
        document.getElementById('meta').textContent='Connection lost';
      }
    }
    setInterval(refresh,900);
    refresh();
  </script>
</body>
</html>
)rawliteral";

    web_server.send(200, "text/html", html);
  });

  web_server.on("/status", []() {
    String json = "{";
    json += "\"label\":";
    appendJsonString(json, latest_label);
    json += ",\"confidence\":";
    json += String(latest_confidence, 4);
    json += ",\"raw\":";
    appendJsonString(json, latest_raw_label);
    json += ",\"normalized\":";
    appendJsonString(json, latest_normalized_label);
    json += ",\"candidate\":";
    appendJsonString(json, latest_candidate_label);
    json += ",\"monitor\":";
    appendJsonString(json, latest_monitor_line);
    json += ",\"frames_seen\":";
    json += String(static_cast<unsigned int>(latest_frame_seen));
    json += ",\"sample_frames\":";
    json += String(static_cast<unsigned int>(kDetectionSampleFrames));
    json += ",\"required_frames\":";
    json += String(static_cast<unsigned int>(kRequiredMatchingFrames));
    json += ",\"fallback_frames\":";
    json += String(static_cast<unsigned int>(kFallbackMatchingFrames));
    json += ",\"blocked\":";
    json += latest_blocked ? "true" : "false";
    json += ",\"sequence\":";
    json += String(sequence_id);
    json += ",\"uptime_ms\":";
    json += String(latest_uptime_ms);
    json += ",\"active\":";
    json += detection_active ? "true" : "false";
    json += ",\"preview\":";
    json += preview_active ? "true" : "false";
    json += ",\"cameraOn\":";
    json += cameraOutputActive() ? "true" : "false";
    json += ",\"led\":";
    json += digitalRead(kLedPin) == HIGH ? "true" : "false";
    json += ",\"ip\":";
    appendJsonString(json, WiFi.localIP().toString().c_str());
    json += "}";
    web_server.send(200, "application/json", json);
  });

  web_server.on("/snapshot.jpg", []() {
    handleSnapshotRequest();
  });

  web_server.on("/preview/start", []() {
    setPreviewActive(true, "web/app preview");
    web_server.send(200, "text/plain", "Preview started");
  });

  web_server.on("/preview/stop", []() {
    setPreviewActive(false, "web/app preview");
    web_server.send(200, "text/plain", "Preview stopped");
  });

  web_server.on("/detect/start", []() {
    setDetectionActive(true, "web test");
    web_server.send(200, "text/plain", "Detection started");
  });

  web_server.on("/detect/stop", []() {
    setDetectionActive(false, "web test");
    web_server.send(200, "text/plain", "Detection stopped");
  });

  web_server.on("/led/on", []() {
    digitalWrite(kLedPin, HIGH);
    web_server.send(200, "text/plain", "LED is ON");
  });

  web_server.on("/led/off", []() {
    if (!cameraOutputActive()) {
      digitalWrite(kLedPin, LOW);
    }
    web_server.send(200, "text/plain", digitalRead(kLedPin) == HIGH ? "LED is ON" : "LED is OFF");
  });

  web_server.begin();
  Serial.println("Web server started");
}

bool initCamera() {
  camera_config.ledc_channel = LEDC_CHANNEL_0;
  camera_config.ledc_timer = LEDC_TIMER_0;
  camera_config.pin_d0 = 5;
  camera_config.pin_d1 = 18;
  camera_config.pin_d2 = 19;
  camera_config.pin_d3 = 21;
  camera_config.pin_d4 = 36;
  camera_config.pin_d5 = 39;
  camera_config.pin_d6 = 34;
  camera_config.pin_d7 = 35;
  camera_config.pin_xclk = 0;
  camera_config.pin_pclk = 22;
  camera_config.pin_vsync = 25;
  camera_config.pin_href = 23;
  camera_config.pin_sccb_sda = 26;
  camera_config.pin_sccb_scl = 27;
  camera_config.pin_pwdn = 32;
  camera_config.pin_reset = -1;
  camera_config.xclk_freq_hz = 20000000;
  camera_config.pixel_format = PIXFORMAT_RGB565;
  camera_config.frame_size = psramFound() ? FRAMESIZE_QVGA : FRAMESIZE_QQVGA;
  camera_config.jpeg_quality = 12;
  camera_config.fb_count = psramFound() ? 2 : 1;
  camera_config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    camera_config.fb_location = CAMERA_FB_IN_PSRAM;
  }

  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK) {
    Serial.printf("esp_camera_init() failed: 0x%x\n", err);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_contrast(sensor, 1);
    sensor->set_saturation(sensor, 1);
    sensor->set_whitebal(sensor, 1);
    sensor->set_awb_gain(sensor, 1);
    sensor->set_gain_ctrl(sensor, 1);
    sensor->set_exposure_ctrl(sensor, 1);
    sensor->set_aec2(sensor, 1);
    sensor->set_lenc(sensor, 1);
  }

  return true;
}

bool initTflm() {
  model = tflite::GetModel(g_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("TFLite schema version mismatch");
    return false;
  }

  tensor_arena = static_cast<uint8_t*>(
    psramFound() ? ps_malloc(kTensorArenaSize) : malloc(kTensorArenaSize)
  );
  if (tensor_arena == nullptr) {
    Serial.println("Tensor arena allocation failed");
    return false;
  }

  static tflite::MicroMutableOpResolver<12> resolver;
  resolver.AddQuantize();
  resolver.AddDequantize();
  resolver.AddConv2D();
  resolver.AddDepthwiseConv2D();
  resolver.AddAveragePool2D();
  resolver.AddFullyConnected();
  resolver.AddReshape();
  resolver.AddSoftmax();
  resolver.AddMul();
  resolver.AddAdd();
  resolver.AddPad();
  resolver.AddMean();

  static tflite::MicroInterpreter static_interpreter(
    model, resolver, tensor_arena, kTensorArenaSize
  );
  interpreter = &static_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("AllocateTensors() failed");
    return false;
  }

  input_tensor = interpreter->input(0);
  output_tensor = interpreter->output(0);

  if (input_tensor->type != kTfLiteUInt8 && input_tensor->type != kTfLiteFloat32) {
    Serial.printf("Unsupported input tensor type: %d\n", input_tensor->type);
    return false;
  }

  Serial.print("Tensor arena bytes: ");
  Serial.println(kTensorArenaSize);
  return true;
}

void preprocessFrame(camera_fb_t* fb, TfLiteTensor* input) {
  const int src_w = fb->width;
  const int src_h = fb->height;
  const RoiRect roi = computeRoiRect(src_w, src_h);

  for (int y = 0; y < kInputSize; ++y) {
    int src_y = roi.y + (y * roi.h) / kInputSize;
    for (int x = 0; x < kInputSize; ++x) {
      int src_x = roi.x + (x * roi.w) / kInputSize;
      int index = (src_y * src_w + src_x) * 2;
      uint16_t pixel = static_cast<uint16_t>(fb->buf[index]) |
                       (static_cast<uint16_t>(fb->buf[index + 1]) << 8);

      uint8_t r = ((pixel >> 11) & 0x1F) * 255 / 31;
      uint8_t g = ((pixel >> 5) & 0x3F) * 255 / 63;
      uint8_t b = (pixel & 0x1F) * 255 / 31;

      int dst_index = (y * kInputSize + x) * 3;
      if (input->type == kTfLiteFloat32) {
        float* dst = input->data.f;
        dst[dst_index + 0] = static_cast<float>(r) / 255.0f;
        dst[dst_index + 1] = static_cast<float>(g) / 255.0f;
        dst[dst_index + 2] = static_cast<float>(b) / 255.0f;
      } else {
        uint8_t* dst = input->data.uint8;
        dst[dst_index + 0] = r;
        dst[dst_index + 1] = g;
        dst[dst_index + 2] = b;
      }
    }
  }
}

float dequantizeValue(const TfLiteTensor* tensor, int index) {
  const TfLiteAffineQuantization* quant =
    reinterpret_cast<const TfLiteAffineQuantization*>(tensor->quantization.params);
  if (quant == nullptr || quant->scale == nullptr || quant->zero_point == nullptr) {
    return static_cast<float>(tensor->data.uint8[index]);
  }

  const float scale = quant->scale->data[0];
  const int zero_point = quant->zero_point->data[0];

  if (tensor->type == kTfLiteUInt8) {
    return (static_cast<int>(tensor->data.uint8[index]) - zero_point) * scale;
  }
  if (tensor->type == kTfLiteInt8) {
    return (static_cast<int>(tensor->data.int8[index]) - zero_point) * scale;
  }
  return tensor->data.f[index];
}

void findTopPrediction(int& best_index, float& best_probability) {
  float max_logit = -1e9f;
  float exp_sum = 0.0f;

  best_index = 0;
  best_probability = 0.0f;

  for (int i = 0; i < g_num_classes; ++i) {
    float logit = dequantizeValue(output_tensor, i);
    if (logit > max_logit) {
      max_logit = logit;
      best_index = i;
    }
  }

  for (int i = 0; i < g_num_classes; ++i) {
    exp_sum += expf(dequantizeValue(output_tensor, i) - max_logit);
  }

  if (exp_sum <= 0.0f) {
    best_probability = 0.0f;
    return;
  }

  best_probability = expf(dequantizeValue(output_tensor, best_index) - max_logit) / exp_sum;
}

void recordDetectionVote(const char* label, float confidence) {
  detection_frames_seen++;
  if (strcmp(label, "Unknown") == 0) {
    return;
  }

  for (uint8_t i = 0; i < kDetectionSampleFrames; ++i) {
    if (strcmp(detection_votes[i].label, label) == 0) {
      detection_votes[i].count++;
      detection_votes[i].confidence_sum += confidence;
      if (confidence > detection_votes[i].best_confidence) {
        detection_votes[i].best_confidence = confidence;
      }
      return;
    }
  }

  for (uint8_t i = 0; i < kDetectionSampleFrames; ++i) {
    if (detection_votes[i].count == 0) {
      strlcpy(detection_votes[i].label, label, sizeof(detection_votes[i].label));
      detection_votes[i].count = 1;
      detection_votes[i].confidence_sum = confidence;
      detection_votes[i].best_confidence = confidence;
      return;
    }
  }
}

bool bestDetectionVote(char* label, size_t label_size, float& average_confidence, uint8_t& count) {
  const DetectionVote* best_vote = nullptr;
  for (uint8_t i = 0; i < kDetectionSampleFrames; ++i) {
    if (detection_votes[i].count == 0) {
      continue;
    }
    if (best_vote == nullptr ||
        detection_votes[i].count > best_vote->count ||
        (detection_votes[i].count == best_vote->count &&
         detection_votes[i].best_confidence > best_vote->best_confidence)) {
      best_vote = &detection_votes[i];
    }
  }

  if (best_vote == nullptr) {
    label[0] = '\0';
    average_confidence = 0.0f;
    count = 0;
    return false;
  }

  strlcpy(label, best_vote->label, label_size);
  average_confidence = best_vote->confidence_sum / best_vote->count;
  count = best_vote->count;
  return true;
}

void sendDetection(const char* label, float confidence) {
  DetectionPacket packet = {};
  packet.packetType = kPacketTypeDetectionResult;
  strlcpy(packet.label, label, sizeof(packet.label));
  packet.confidence = confidence;
  packet.sequence = ++sequence_id;
  packet.uptime_ms = millis();

  strlcpy(latest_candidate_label, label, sizeof(latest_candidate_label));
  if (strcmp(label, "Unknown") != 0) {
    latest_blocked = false;
  }
  snprintf(
    latest_monitor_line,
    sizeof(latest_monitor_line),
    "Sent: %s | confidence=%.4f | sequence=%lu",
    packet.label,
    packet.confidence,
    static_cast<unsigned long>(packet.sequence)
  );

  Serial.print("Sending: ");
  Serial.print(packet.label);
  Serial.print(" | confidence=");
  Serial.println(packet.confidence, 4);

  esp_err_t result = esp_now_send(kBroadcastPeer, reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
  if (result != ESP_OK) {
    Serial.printf("esp_now_send() failed: %d\n", result);
  }

  detection_result_sent = true;
  setDetectionActive(false, "result sent");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println(kDeviceName);

  pinMode(kLedPin, OUTPUT);
  digitalWrite(kLedPin, LOW);

  if (!connectWifi()) {
    Serial.println("Camera will keep running, but ESP-NOW may not share the scale channel.");
  }

  if (!initEspNow()) {
    while (true) {
      delay(1000);
    }
  }

  if (!initCamera()) {
    while (true) {
      delay(1000);
    }
  }
  camera_ready = true;

  if (!initTflm()) {
    while (true) {
      delay(1000);
    }
  }
  model_ready = true;
  esp_now_ready = true;

  startWebServer();
  setDetectionActive(false, "startup idle");
}

void loop() {
  web_server.handleClient();

  if (preview_active && millis() - last_preview_ms >= kPreviewIdleTimeoutMs) {
    setPreviewActive(false, "preview idle timeout");
  }

  if (!detection_active) {
    delay(10);
    return;
  }

  if (millis() - detection_started_ms >= kDetectionTimeoutMs) {
    strlcpy(latest_label, "Unknown", sizeof(latest_label));
    latest_confidence = 0.0f;
    latest_uptime_ms = millis();
    sendDetection("Unknown", 0.0f);
    return;
  }

  if (millis() - detection_started_ms < kPlacementSettleMs) {
    strlcpy(latest_label, "Settling", sizeof(latest_label));
    latest_confidence = 0.0f;
    latest_uptime_ms = millis();
    delay(10);
    return;
  }

  if (last_detection_ms != 0 &&
      millis() - last_detection_ms < kDetectionIntervalMs) {
    delay(10);
    return;
  }
  last_detection_ms = millis();

  if (!camera_ready || !model_ready || !esp_now_ready) {
    sendDetection("Unknown", 0.0f);
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    Serial.println("Camera capture failed");
    strlcpy(latest_monitor_line, "Camera capture failed", sizeof(latest_monitor_line));
    delay(250);
    return;
  }

  preprocessFrame(fb, input_tensor);
  esp_camera_fb_return(fb);

  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("Inference failed");
    strlcpy(latest_monitor_line, "Inference failed", sizeof(latest_monitor_line));
    delay(250);
    return;
  }

  int best_index = 0;
  float best_probability = 0.0f;
  findTopPrediction(best_index, best_probability);

  const char* raw_label = mapLabel(g_class_labels[best_index]);
  const char* normalized_label = normalizeFruitName(raw_label);
  const char* predicted_label = isAllowedFruitName(normalized_label) ? normalized_label : "Unknown";
  const bool blocked_label = strcmp(predicted_label, "Unknown") == 0 &&
                             strcmp(normalized_label, "Unknown") != 0;
  recordDetectionVote(predicted_label, best_probability);
  strlcpy(latest_raw_label, raw_label, sizeof(latest_raw_label));
  strlcpy(latest_normalized_label, normalized_label, sizeof(latest_normalized_label));
  strlcpy(latest_candidate_label, predicted_label, sizeof(latest_candidate_label));
  latest_frame_seen = detection_frames_seen;
  latest_blocked = blocked_label;

  strlcpy(latest_label, "Processing", sizeof(latest_label));
  latest_confidence = best_probability;
  latest_uptime_ms = millis();

  snprintf(
    latest_monitor_line,
    sizeof(latest_monitor_line),
    "Detected: %s raw=%s%s%s | confidence=%.4f | frame=%u/%u",
    predicted_label,
    raw_label,
    blocked_label ? " blocked=" : "",
    blocked_label ? normalized_label : "",
    best_probability,
    static_cast<unsigned int>(latest_frame_seen),
    static_cast<unsigned int>(kDetectionSampleFrames)
  );

  Serial.print("Detected: ");
  Serial.print(predicted_label);
  Serial.print(" raw=");
  Serial.print(raw_label);
  if (blocked_label) {
    Serial.print(" blocked=");
    Serial.print(normalized_label);
  }
  Serial.print(" | confidence=");
  Serial.print(best_probability, 4);
  Serial.print(" | frame=");
  Serial.print(detection_frames_seen);
  Serial.print("/");
  Serial.println(kDetectionSampleFrames);

  char stable_label[32] = "";
  float stable_confidence = 0.0f;
  uint8_t stable_count = 0;
  if (bestDetectionVote(stable_label, sizeof(stable_label), stable_confidence, stable_count) &&
      stable_count >= kRequiredMatchingFrames) {
    strlcpy(latest_label, stable_label, sizeof(latest_label));
    latest_confidence = stable_confidence;
    sendDetection(stable_label, stable_confidence);
    return;
  }

  if (detection_frames_seen >= kDetectionSampleFrames) {
    if (stable_count >= kFallbackMatchingFrames) {
      strlcpy(latest_label, stable_label, sizeof(latest_label));
      latest_confidence = stable_confidence;
      latest_blocked = false;
      sendDetection(stable_label, stable_confidence);
      return;
    }

    strlcpy(latest_label, "Unknown", sizeof(latest_label));
    latest_confidence = 0.0f;
    latest_uptime_ms = millis();
    sendDetection("Unknown", 0.0f);
  }
}
