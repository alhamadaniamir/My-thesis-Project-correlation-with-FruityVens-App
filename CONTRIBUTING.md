# Contributing

Thanks for your interest in contributing to the **Fruit Vens** ESP32 firmware — the computer-vision fruit detection IoT weighing scale. This guide explains how to set up your environment, the conventions to follow, and how to submit changes.

## Getting Started

1. **Fork** the repository and clone your fork.
2. Install the toolchain (see [README.md](README.md) → *Build & Flash*):
   - Arduino IDE with the **ESP32 board package** (Espressif).
   - Libraries: `hd44780`, `HX711_ADC`, `RTClib`.
3. Create a branch off `main`:
   ```bash
   git checkout -b feature/short-description
   ```

## Project Layout

| Folder | Board | Purpose |
|---|---|---|
| `AutomatedWeighing/` | ESP32 Dev Module | Scale, LCD, buttons, sale logic |
| `ESP32CamSender/` | ESP32-CAM | Camera capture + Gemini backend call |
| `ESP32Receiver/` | ESP32 Dev Module | Firebase upload worker (optional) |

Each sketch keeps tunables and secrets in its own `config.h` / `config.cpp`. The shared ESP-NOW packet definitions live in each sketch's `protocol.h` — **keep these three copies identical** when you change a packet layout, or the boards will stop understanding each other.

## Coding Conventions

- **Match the surrounding style.** This codebase uses 2-space indentation, `camelCase` for functions/variables, `k`-prefixed `constexpr` for camera/worker config, and `UPPER_SNAKE_CASE` `extern const` for scale config.
- **No blocking in the scale loop.** `AutomatedWeighing` must keep servicing the HX711; never add long `delay()`s or synchronous network calls to its main path. Offload network work or guard it behind timeouts/backoff like the existing Firebase code.
- **Keep ESP-NOW packets in sync** across all three `protocol.h` files. Bump the packet `type` constant if you change a layout.
- **Use the existing helpers** (`strlcpy`, `snprintf`, the JSON helpers in `json_utils`) rather than introducing new string handling.
- **Never commit real secrets.** WiFi passwords, Firebase tokens, and backend auth tokens live in `config` files — scrub them before opening a PR, or move them to placeholders.

## Testing Your Changes

Before submitting:

1. **Compile** each sketch you touched in the Arduino IDE (no warnings introduced).
2. **Flash and observe** on real hardware where possible — watch the Serial Monitor at 115200 baud.
3. For scale changes, verify the weight lock, LCD display, and sale flow still work.
4. For camera changes, confirm a successful round-trip: object on scale → `Identifying` → fruit name on LCD.
5. Note in your PR what hardware you tested on (board, sensors connected).

## Commit Messages

- Use clear, imperative subject lines: `Fix HX711 noise filter on fast weight changes`.
- Reference the affected sketch when relevant: `ESP32CamSender: retry backend on timeout`.
- Keep unrelated changes in separate commits.

## Submitting a Pull Request

1. Push your branch and open a PR against `main`.
2. Describe **what** changed, **why**, and **how you tested it**.
3. Include serial logs or photos for hardware-facing changes when helpful.
4. Be responsive to review feedback.

## Reporting Issues

When filing a bug, include:

- Which board/sketch is affected.
- Serial Monitor output (115200 baud).
- Steps to reproduce and what you expected vs. what happened.
- Your hardware setup (sensors, wiring deviations from the README).

## License

By contributing, you agree that your contributions will be licensed under the [Apache License 2.0](LICENSE) that covers this project.
