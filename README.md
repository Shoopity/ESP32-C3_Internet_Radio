# ESP32-C3 Super Mini Internet Radio

A lightweight internet radio streamer designed for the ESP32-C3 Super Mini and the NS4168 I2S Mono Amplifier.

## Hardware
- **MCU**: ESP32-C3 Super Mini
- **Amplifier**: NS4168 I2S Mono Audio Amplifier
- **Power**: USB-C

## Wiring
| NS4168 Pin | ESP32-C3 Pin |
|------------|--------------|
| V (5V)     | 5V           |
| G (GND)    | GND          |
| BCLK       | GPIO 1       |
| LRC        | GPIO 2       |
| DIN        | GPIO 3       |

## Software Configuration
This project is built using **PlatformIO**. To ensure stability, the following versions are pinned in `platformio.ini`:
- **Platform**: `espressif32@~6.7.0` (Arduino 2.x)
- **Library**: `earlephilhower/ESP8266Audio@1.9.7`

## Setup
1. Clone the repository.
2. Open in VS Code with PlatformIO.
3. Update your WiFi credentials in `src/main.cpp`.
4. Flash the board (use `--no-stub` if you encounter connection errors).

## Special Note on Flashing
If you encounter `No serial data received` errors during upload:
- Hold the **BOOT** button.
- Tap **RESET**.
- Release **BOOT**.
- Click **Upload**.
