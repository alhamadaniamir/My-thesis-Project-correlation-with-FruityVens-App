# Fruit Vens Backend

Serverless Rust backend deployed on Vercel that powers the fruit identification feature of the **Fruit Vending Scale** system — an automated fruit weighing and point-of-sale system built around ESP32 microcontrollers.

## What It Does

When a fruit is placed on the scale, the **ESP32-CAM** captures a JPEG image and sends it to this backend via HTTPS. The backend forwards the image to **Google Gemini AI**, which identifies the fruit and returns one label from a fixed list (e.g. `Orange`, `Mango Carabao`, `Apple`). The result is sent back to the ESP32-CAM, which broadcasts it over **ESP-NOW** to the main scale board, where it is displayed on the **LCD I2C screen** alongside the weight and computed price.

## System Architecture

```
[Fruit placed on scale]
        │
        ▼
[HX711 Load Cell] ──► [AutomatedWeighing ESP32]
                              │  ESP-NOW "START"
                              ▼
                    [ESP32-CAM]
                         │  captures JPEG
                         ▼
              [This Vercel Backend] ──► [Google Gemini API]
                         │  {"fruit": "Mango Carabao"}
                         ▼
                    [ESP32-CAM]
                         │  ESP-NOW DetectionPacket
                         ▼
             [AutomatedWeighing ESP32]
                         │
                         ▼
               [LCD I2C 20x4 Display]
               Fruit: Mango Carabao
               Wt: 350g   P: 24.50
```

### Step-by-Step Flow

**1. Fruit placed on scale**
The user places a fruit on the platform of the digital weighing scale. This is the trigger event for the entire pipeline.

**2. HX711 Load Cell → AutomatedWeighing ESP32**
The load cell is a strain gauge that flexes under weight and produces a tiny analog voltage change. The HX711 is a 24-bit ADC that amplifies and digitizes that signal, then streams it over a 2-wire serial protocol (`DOUT`/`SCK`) to the main ESP32. The AutomatedWeighing ESP32 reads this stream continuously, applies an exponential moving average filter (`WEIGHT_FILTER_ALPHA`) to smooth noise, and detects when the weight crosses the `OBJECT_DETECT_GRAMS` threshold and stays stable.

**3. ESP-NOW "START" command**
Once the scale confirms a stable object is on it (after `CAMERA_START_DELAY_MS`), it broadcasts a `ScaleCommandPacket` with the string `"START"` over **ESP-NOW** — Espressif's peer-to-peer 2.4 GHz protocol that requires no router and has ~2 ms latency. The LCD updates to show `Fruit: Identifying` to signal the user that detection has begun.

**4. ESP32-CAM captures JPEG**
The ESP32-CAM (AI Thinker module with an OV2640 camera) receives the START packet. It waits `kPlacementSettleMs` (~1.2 s) for the fruit to be still, then triggers the OV2640 to capture a frame. The frame is cropped to a region of interest (`kRoiX/Y/W/H` — 82% × 80% of the center) and JPEG-encoded at quality `kCaptureJpegQuality = 14` to keep the payload small.

**5. HTTPS POST → Vercel Backend (Singapore)**
The ESP32-CAM base64-encodes the JPEG and POSTs it to `/api/identify`. The request includes `Authorization: Bearer <token>` so the backend can reject unauthorized callers. Vercel routes the request to a serverless Rust function running in the `sin1` (Singapore) region — closest to a Philippines deployment, minimizing round-trip latency (~5 s including Gemini).

**6. Backend → Google Gemini API (WebSocket)**
The Rust backend opens a WebSocket to `generativelanguage.googleapis.com` using the live `BidiGenerateContent` endpoint. It sends a setup message, then a `clientContent` message containing the prompt + base64 JPEG. The prompt instructs Gemini to answer with exactly one label from a fixed allow list. Generic `Mango` is forbidden: a mango must be classified as `Mango Carabao`, `Indian Mango`, or `Apple Mango`, and anything the model cannot place confidently becomes `Unknown`. Gemini streams back its identification; the backend collects it until `turnComplete` arrives.

**7. Response `{"fruit": "Mango Carabao"}`**
The backend runs the transcript through `canonical_fruit()`, which strips punctuation and casing, matches the longest known alias, and falls back to `Unknown`. So a reply of "Carabao mango." still becomes `Mango Carabao`, and a bare "Mango" becomes `Unknown` instead of leaking a generic label. The backend returns a minimal JSON: `{"fruit": "Mango Carabao"}`. The ESP32-CAM re-checks the label against its own allow list before packing it into a `DetectionPacket`.

**8. ESP-NOW DetectionPacket back to scale**
The ESP32-CAM broadcasts the `DetectionPacket` containing `label`, `confidence`, and a sequence number. The AutomatedWeighing ESP32 receives it in `onEspNowReceived()`, verifies `confidence >= FRUIT_DETECTION_CONFIDENCE`, and writes the label into `currentFruitType`.

**9. LCD I2C 20×4 Display**
The scale's `updateDisplay()` function refreshes every `DISPLAY_INTERVAL_MS` and renders 4 lines:
- **Line 0**: `Fruit: Mango Carabao` — taken from `currentFruitType`
- **Line 1**: `Wt: 350g   P: 24.50` — weight + price (price = weight_kg × `pricePerKgForFruit(currentFruitType)`)
- **Line 2**: `Stat: L 14:32` — status flag (L=locked, W=weighing, Z=zero) + RTC clock
- **Line 3**: `05/28/2026` — current date from DS3231 RTC

The LCD uses I2C at address `0x27` on pins `SDA=18`, `SCL=21`.

### What Happens After the Sale
- User presses the **green button** → scale calls `confirmSale()` → builds a `SaleRecord` with weight, price, fruit, and RTC timestamp → enqueues it.
- The sale is either uploaded directly to **Firebase Realtime Database** by the scale itself, **or** forwarded over ESP-NOW to the optional **ESP32Receiver worker board** which has stronger WiFi and dedicated upload logic.
- Prices flow the other direction: a web/admin app writes new prices to Firebase, the scale (or worker) polls `scalePriceUpdates/<deviceId>/latest`, broadcasts `PriceUpdatePacket`s, and `prices.cpp` on the scale updates its in-memory price table.

### Why the 3-Board Split?
| Board | Strength | Reason |
|---|---|---|
| AutomatedWeighing ESP32 | Real-time HX711 reads, GPIO, LCD | Can't afford WiFi blocking the weight loop |
| ESP32-CAM | Camera + PSRAM | Only board with image capture capability |
| Worker ESP32 (optional) | WiFi/HTTPS uploads | Isolates Firebase latency from the scale UI |

ESP-NOW glues all three together at the link layer so they can talk without a WiFi router being present.

## Tech Stack

- **Language**: Rust (2024 edition)
- **Runtime**: Vercel Serverless Functions via [`vercel_runtime`](https://crates.io/crates/vercel_runtime) with Axum
- **AI**: Google Gemini API over WebSocket (`BidiGenerateContent`)
- **Auth**: Bearer token header (`Authorization: Bearer <token>`)
- **Region**: Singapore (`sin1`) for low latency in Southeast Asia
- **TLS**: rustls with webpki roots

## API

### `POST /api/identify`

Identifies the fruit in a provided image.

**Headers**
```
Content-Type: application/json
Authorization: Bearer <AUTH_TOKEN>
```

**Request Body**
```json
{
  "mime_type": "image/jpeg",
  "data": "<base64-encoded image>"
}
```

**Response**
```json
{
  "fruit": "Mango Carabao"
}
```

The response is always one of these labels:

`Apple`, `Mango Carabao`, `Indian Mango`, `Apple Mango`, `Dragon Fruit`, `Watermelon`, `Pineapple`, `Mangosteen`, `Banana`, `Avocado`, `Grapes`, `Durian`, `Pomelo`, `Pear`, `Lemon`, `Orange`, `Unknown`

- Generic `Mango` is never returned. A mango is reported as `Mango Carabao`, `Indian Mango`, or `Apple Mango`.
- Close relatives collapse into their list entry (tangerine and clementine become `Orange`, lime becomes `Lemon`, pitaya becomes `Dragon Fruit`).
- Returns `Unknown` if no fruit is detected, the fruit is not on the list, or the mango variety is not clear.

## Environment Variables

| Variable | Description |
|---|---|
| `GEMINI_API_KEY` | Google Gemini API key |
| `AUTH_TOKEN` | Bearer token required by ESP32-CAM to authenticate requests |
| `CORS_ORIGIN` | Allowed CORS origin (`*` for all, or a specific URL) |

Set these in Vercel dashboard or via CLI:
```bash
echo "your-key" | vercel env add GEMINI_API_KEY production
echo "your-token" | vercel env add AUTH_TOKEN production
echo "*" | vercel env add CORS_ORIGIN production
```

## Deployment

```bash
# Deploy to production
vercel --prod --yes

# The stable production URL is:
# https://backend-kappa-roan-83.vercel.app
```

## ESP32 Integration

The ESP32-CAM sketch (`ESP32CamSender`) points to this backend via `kBackendUrl` in `config.h`:

```cpp
constexpr char kBackendUrl[] = "https://backend-kappa-roan-83.vercel.app/api/identify";
constexpr char kBackendAuthToken[] = "<AUTH_TOKEN>";
constexpr uint32_t kBackendHttpTimeoutMs = 15000;
```

The sketch base64-encodes the captured JPEG and POST-s it to `/api/identify`. The returned fruit name is then normalized, broadcast over ESP-NOW, and displayed on the LCD.

## Related Hardware

| Component | Role |
|---|---|
| ESP32-CAM (AI Thinker) | Captures image, calls this backend, broadcasts result |
| ESP32 (main scale) | HX711 load cell, LCD I2C 20×4, buttons, buzzer, RTC DS3231 |
| ESP32 (worker, optional) | Firebase upload worker, price update relay |
| HX711 | Load cell amplifier for weight measurement |
| LCD I2C 20×4 (0x27) | Displays fruit name, weight, price, time |
| RTC DS3231 | Real-time clock for sale timestamps |
