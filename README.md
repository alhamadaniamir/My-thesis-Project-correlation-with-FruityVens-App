# Fruit Vens — ESP32 Firmware

**Computer Vision-Based Fruit Detection Automated IoT Weighing Scale with AI-Driven Sales Forecasting and Data Analytics**

This folder contains the firmware for the three ESP32 boards that make up the smart fruit weighing scale. The boards talk to each other over **ESP-NOW** (router-free peer-to-peer) and to the cloud over **WiFi** (Google Gemini via the Vercel backend, and Firebase Realtime Database).

## Boards & Sketches

| Sketch | Target Board | Role |
|---|---|---|
| [`AutomatedWeighing/`](AutomatedWeighing) | ESP32 Dev Module | Main scale: NAU7802 load-cell ADC, LCD I2C, buttons, buzzer, RTC. Detects weight, requests fruit ID, displays result, records sales. |
| [`ESP32CamSender/`](ESP32CamSender) | ESP32-CAM (AI Thinker) | Camera: captures JPEG, calls the Vercel/Gemini backend, broadcasts the fruit name. |
| [`ESP32Receiver/`](ESP32Receiver) | ESP32 Dev Module | Firebase worker (optional): uploads sales to Firebase and relays price updates. |

## Master / Slave Roles

Although ESP-NOW is technically peer-to-peer (every board broadcasts to every other), the system has a clear logical hierarchy:

| Board | Role | Why |
|---|---|---|
| **AutomatedWeighing ESP32** | 🟢 **MASTER** (controller) | It owns the whole workflow. It decides *when* to start/stop detection, holds the authoritative weight + sale state, drives the LCD, and is the single source of truth for a transaction. Nothing happens until the master tells it to. |
| **ESP32-CAM** | 🔵 **SLAVE** (sensor) | It does nothing on its own. It sits idle until the master sends `START`, then captures + identifies and reports the result back. It never initiates a transaction. |
| **ESP32Receiver** | 🔵 **SLAVE** (worker) | A passive helper. It listens for `SaleSync` packets from the master, uploads them to Firebase, acknowledges them, and relays price updates back. Optional — only used when `useFirebaseWorkerEsp32 = true`. |

**Command flow (who tells whom):**

```
                 MASTER
          [AutomatedWeighing ESP32]
            │                  │
   "START"/"STOP"          "SaleSync"
            │                  │
            ▼                  ▼
       [ESP32-CAM]       [ESP32Receiver]
        (SLAVE)             (SLAVE)
            │                  │
   "DetectionPacket"    "SaleAck" / "PriceUpdate"
            │                  │
            └────► back to MASTER ◄────┘
```

- The **master** is the only board that initiates actions (start detection, record sale).
- The **slaves** only ever *respond* to the master and report results back.
- All three communicate by ESP-NOW broadcast, so they must share the same WiFi channel (inherited from the common router SSID).

## How It Works

```
[Fruit on scale]
      │
      ▼
[NAU7802] ─► [AutomatedWeighing ESP32] ──ESP-NOW "START"──► [ESP32-CAM]
                    ▲                                          │ capture JPEG
                    │                                          ▼
                    │                                 [Vercel Backend] ─► [Gemini AI]
                    │                                          │ {"fruit":"Mango"}
                    │◄──────ESP-NOW DetectionPacket────────────┘
                    │
                    ├─► [LCD I2C 20x4]  Fruit: Mango / Wt: 350g  P: 24.50
                    │
                    └──ESP-NOW SaleSync──► [ESP32Receiver] ─► [Firebase RTDB]
                    ◄──ESP-NOW PriceUpdate──┘ (price sync)
```

1. **Weight detected** — `AutomatedWeighing` filters the NAU7802 stream and locks a stable weight above `OBJECT_DETECT_GRAMS` (9 g).
2. **Detection requested** — after `CAMERA_START_DELAY_MS` (1 s) it broadcasts a `"START"` command; the LCD shows `Fruit: Identifying`.
3. **Image captured** — `ESP32CamSender` snaps the cropped ROI as a JPEG.
4. **Fruit identified** — the JPEG is POSTed to the Vercel backend, which asks Gemini and returns `{"fruit":"Mango"}`.
5. **Result broadcast** — the cam normalizes the name and sends a `DetectionPacket` back over ESP-NOW.
6. **Display** — `AutomatedWeighing` shows the fruit, weight, computed price (`weight_kg × price/kg`), status, and time on the LCD.
7. **Sale & sync** — pressing the green button records a sale; it is uploaded to Firebase (directly or via the `ESP32Receiver` worker). Prices set in the cloud flow back as `PriceUpdatePacket`s.

## ESP-NOW Protocol

All boards share the packet definitions in each sketch's `protocol.h`:

| Packet | Type | Direction |
|---|---|---|
| `ScaleCommandPacket` | 1 | Scale → Cam (`START` / `STOP`) |
| `DetectionPacket` | 2 | Cam → Scale (fruit label + confidence) |
| `SaleSyncPacket` | 3 | Scale → Worker (sale record) |
| `PriceUpdatePacket` | 4 | Worker → Scale (price per kg) |
| `SaleAckPacket` | 5 | Worker → Scale (upload confirmed) |

Communication is by broadcast (`FF:FF:FF:FF:FF:FF`), so no MAC pairing is needed — but all boards **must be on the same WiFi channel**, which they inherit from the shared router SSID.

## Wiring (AutomatedWeighing ESP32)

| Peripheral | Pin |
|---|---|
| NAU7802 SDA | GPIO 18 (shared I2C bus) |
| NAU7802 SCL | GPIO 21 (shared I2C bus) |
| NAU7802 VCC / 3V3 | Regulated 3.27 V buck output |
| NAU7802 GND | Buck GND and ESP32 GND (common ground) |
| NAU7802 INT / DRDY | Not connected |
| LCD + RTC SDA | GPIO 18 |
| LCD + RTC SCL | GPIO 21 |
| Success button | GPIO 12 (INPUT_PULLUP) |
| Cancel button | GPIO 14 (INPUT_PULLUP) |
| Buzzer | GPIO 26 |
| LCD I2C address | `0x27` (20×4) |
| RTC | DS3231 on I2C |
| NAU7802 I2C address | `0x2A` |

Typical four-wire load-cell connection:

| Load-cell wire | NAU7802 terminal |
|---|---|
| Red | `E+` |
| Black | `E-` |
| Green | `A+` |
| White | `A-` |

Load-cell colors are not universal; verify the label or datasheet supplied with
your load cell. If weight moves in the wrong direction, swap `A+` and `A-` and
calibrate again.

The 3.27 V buck supply is suitable for the NAU7802. Its ground must be connected
to ESP32 ground so the I2C signals have the same reference. Do not connect the
buck's 3.27 V output and the ESP32 3.3 V output together. Also verify that no
5 V LCD backpack pull-up is pulling SDA or SCL above 3.3 V.

## Configuration

Each sketch has its own config. Update these before flashing:

**`AutomatedWeighing/config.cpp`**
- `ssid` / `password` — WiFi credentials
- `firebaseDatabaseUrl` / `firebaseScaleDeviceId` — Firebase target
- `useFirebaseWorkerEsp32` — `true` = offload uploads to `ESP32Receiver`; `false` = scale uploads directly (no worker board needed)
- `DEFAULT_CALIBRATION_FACTOR` — NAU7802 startup factor; perform a new calibration after replacing the amplifier
- `gmtOffset_sec` — `8 * 3600` for PH time (UTC+8)

**`ESP32CamSender/config.h`**
- `kWifiSsid` / `kWifiPassword`
- `kBackendUrl` — Vercel endpoint (`https://backend-kappa-roan-83.vercel.app/api/identify`)
- `kBackendAuthToken` — must match the backend's `AUTH_TOKEN`

**`ESP32Receiver/config.h`**
- `kWifiSsid` / `kWifiPassword`
- `kFirebaseDatabaseUrl` / `kFirebaseScaleDeviceId`

## Build & Flash (Arduino IDE)

1. Install the **ESP32 board package** (Boards Manager → "esp32" by Espressif).
2. Install required libraries via Library Manager:
   - `hd44780` (LCD I2C)
   - `SparkFun Qwiic Scale NAU7802 Arduino Library`
   - `RTClib`
3. Select the correct board per sketch:
   - `AutomatedWeighing` / `ESP32Receiver` → **ESP32 Dev Module**
   - `ESP32CamSender` → **AI Thinker ESP32-CAM** (and use an FTDI adapter; GPIO0 → GND to enter flash mode)
4. Open the sketch folder, set credentials in its config, then **Upload**.
5. Open Serial Monitor at **115200 baud** to watch diagnostics.

## Serial Commands (AutomatedWeighing)

| Command | Action |
|---|---|
| `t` | Tare / zero the scale |
| `+` / `-` | Increase / decrease calibration factor |
| `r` | Reset calibration to default |
| `c 500` | Add a known 500 g calibration sample |
| `cal save` | Save the new calibration factor |
| `fruit Mango` | Manually set the fruit name (skips camera) |

After installing the NAU7802, calibrate it before recording sales:

1. Power on with the platform completely empty; startup performs an automatic tare.
2. Open Serial Monitor at 115200 baud and send `t` once more with the platform empty.
3. Place a known weight, such as exactly 500 g, in the center and wait for it to settle.
4. Send `c 500`, then send `cal save`.
5. Remove the weight, send `t`, and verify several known weights.

## Key Tuning Parameters (AutomatedWeighing)

| Constant | Value | Meaning |
|---|---|---|
| `OBJECT_DETECT_GRAMS` | 9 g | Minimum weight to count as an object |
| `OBJECT_REMOVE_GRAMS` | 4 g | Residual weight treated as removed |
| `OBJECT_REDETECT_COOLDOWN_MS` | 2000 ms | Delay before detecting another object after removal |
| `LOCK_MATCH_SAMPLES` | 10 | Stable samples required to lock a weight |
| `CAMERA_START_DELAY_MS` | 1000 ms | Delay before requesting detection |
| `CAMERA_DETECTION_TIMEOUT_MS` | 22000 ms | Retry camera detection after this |
| `FRUIT_DETECTION_CONFIDENCE` | 0.0 | Min confidence to accept a label |

## Related

- **Backend**: [fruit-vens-backend](https://github.com/adam-ctrlc/fruit-vens-backend) — Rust serverless function on Vercel that bridges the camera to Google Gemini.
- **Web demo**: `index.html` — a browser drag-and-drop tester for the same `/api/identify` endpoint.
