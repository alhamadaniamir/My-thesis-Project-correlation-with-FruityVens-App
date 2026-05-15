#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_camera.h>

#include "model_data.h"

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

constexpr char kDeviceName[] = "ESP32-CAM";
constexpr char kWebSsid[] = "FruitCam";
constexpr char kWebPassword[] = "12345678";
constexpr int kInputSize = 96;
constexpr size_t kTensorArenaSize = 1024 * 1024;
constexpr uint8_t kEspNowChannel = 1;
constexpr uint32_t kSendIntervalMs = 2000;

// Replace with the MAC address of the receiver ESP32.
  uint8_t kReceiverMac[] = {0xD4, 0xE9, 0xF4, 0xB1, 0x0D, 0x98};

struct DetectionPacket {
  char label[32];
  float confidence;
  uint32_t sequence;
  uint32_t uptime_ms;
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
uint32_t last_send_ms = 0;
WebServer web_server(80);
char latest_label[32] = "None";
float latest_confidence = 0.0f;
uint32_t latest_uptime_ms = 0;

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

bool initEspNow() {
  WiFi.mode(WIFI_AP_STA);

  if (esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("Failed to set WiFi channel");
    return false;
  }

  if (!WiFi.softAP(kWebSsid, kWebPassword, kEspNowChannel)) {
    Serial.println("WiFi access point failed");
    return false;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init() failed");
    return false;
  }

  esp_now_register_send_cb(onSendComplete);

  esp_now_peer_info_t peer_info = {};
  memcpy(peer_info.peer_addr, kReceiverMac, 6);
  peer_info.channel = kEspNowChannel;
  peer_info.encrypt = false;

  if (esp_now_add_peer(&peer_info) != ESP_OK) {
    Serial.println("esp_now_add_peer() failed");
    return false;
  }

  Serial.print("Sender MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Web WiFi: ");
  Serial.println(kWebSsid);
  Serial.print("Web address: http://");
  Serial.println(WiFi.softAPIP());
  Serial.print("Receiver MAC: ");
  Serial.println(macToString(kReceiverMac));
  Serial.print("ESP-NOW channel: ");
  Serial.println(kEspNowChannel);
  return true;
}

void startWebServer() {
  web_server.on("/", []() {
    const char* html =
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
      "<title>FruitCam Live</title>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>"
      "body{font-family:Arial,sans-serif;background:#101820;color:white;margin:0;padding:18px;text-align:center;}"
      "main{max-width:520px;margin:0 auto;}"
      "h2{margin:10px 0 18px;}"
      ".fruit{font-size:42px;font-weight:700;margin:18px 0;word-break:break-word;}"
      ".confidence{font-size:22px;color:#8ee59b;margin-bottom:12px;}"
      ".meta{color:#b8c1cc;font-size:15px;}"
      ".panel{background:#1d2a35;border-radius:10px;padding:18px;}"
      "</style>"
      "</head>"
      "<body>"
      "<main>"
      "<h2>FruitCam Live</h2>"
      "<div class='panel'>"
      "<div class='fruit' id='label'>None</div>"
      "<div class='confidence' id='confidence'>0%</div>"
      "<div class='meta' id='meta'>Waiting for detection...</div>"
      "</div>"
      "</main>"
      "<script>"
      "async function refresh(){"
      "try{"
      "const r=await fetch('/status?ts='+Date.now());"
      "const s=await r.json();"
      "document.getElementById('label').textContent=s.label;"
      "document.getElementById('confidence').textContent=Math.round(s.confidence*100)+'% confidence';"
      "document.getElementById('meta').textContent='Sequence '+s.sequence+' | uptime '+Math.round(s.uptime_ms/1000)+'s';"
      "}catch(e){document.getElementById('meta').textContent='Connection lost';}"
      "}"
      "setInterval(refresh,1000);refresh();"
      "</script>"
      "</body>"
      "</html>";

    web_server.send(200, "text/html", html);
  });

  web_server.on("/status", []() {
    String json = "{";
    json += "\"label\":\"";
    json += latest_label;
    json += "\",\"confidence\":";
    json += String(latest_confidence, 4);
    json += ",\"sequence\":";
    json += String(sequence_id);
    json += ",\"uptime_ms\":";
    json += String(latest_uptime_ms);
    json += "}";
    web_server.send(200, "application/json", json);
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
  camera_config.pin_sscb_sda = 26;
  camera_config.pin_sscb_scl = 27;
  camera_config.pin_pwdn = 32;
  camera_config.pin_reset = -1;
  camera_config.xclk_freq_hz = 20000000;
  camera_config.pixel_format = PIXFORMAT_RGB565;
  camera_config.frame_size = FRAMESIZE_QQVGA;
  camera_config.jpeg_quality = 12;
  camera_config.fb_count = 1;
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
    sensor->set_saturation(sensor, -1);
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
  const int crop = min(src_w, src_h);
  const int x_offset = (src_w - crop) / 2;
  const int y_offset = (src_h - crop) / 2;

  for (int y = 0; y < kInputSize; ++y) {
    int src_y = y_offset + (y * crop) / kInputSize;
    for (int x = 0; x < kInputSize; ++x) {
      int src_x = x_offset + (x * crop) / kInputSize;
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

void sendDetection(const char* label, float confidence) {
  DetectionPacket packet = {};
  strlcpy(packet.label, label, sizeof(packet.label));
  packet.confidence = confidence;
  packet.sequence = ++sequence_id;
  packet.uptime_ms = millis();

  Serial.print("Sending: ");
  Serial.print(packet.label);
  Serial.print(" | confidence=");
  Serial.println(packet.confidence, 4);

  esp_err_t result = esp_now_send(kReceiverMac, reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
  if (result != ESP_OK) {
    Serial.printf("esp_now_send() failed: %d\n", result);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println(kDeviceName);

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

  if (!initTflm()) {
    while (true) {
      delay(1000);
    }
  }

  startWebServer();
}

void loop() {
  web_server.handleClient();

  if (millis() - last_send_ms < kSendIntervalMs) {
    delay(10);
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    Serial.println("Camera capture failed");
    delay(250);
    return;
  }

  preprocessFrame(fb, input_tensor);
  esp_camera_fb_return(fb);

  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("Inference failed");
    delay(250);
    return;
  }

  int best_index = 0;
  float best_probability = 0.0f;
  findTopPrediction(best_index, best_probability);

  const char* predicted_label = mapLabel(g_class_labels[best_index]);
  strlcpy(latest_label, predicted_label, sizeof(latest_label));
  latest_confidence = best_probability;
  latest_uptime_ms = millis();

  Serial.print("Detected: ");
  Serial.print(predicted_label);
  Serial.print(" | confidence=");
  Serial.println(best_probability, 4);

  sendDetection(predicted_label, best_probability);
  last_send_ms = millis();
}
