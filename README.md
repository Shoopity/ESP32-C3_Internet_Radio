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
3. Flash the board (use `--no-stub` if you encounter connection errors).
4. Connect to the setup access point described below and enter your Wi-Fi network and station URLs.

## Codespaces
New Codespaces install the PlatformIO extension, CLI, and project packages automatically through `.devcontainer/devcontainer.json`. The first build may still download the ESP32 toolchain. A cloud Codespace cannot access an ESP32 connected to your local USB port, so use a local VS Code workspace to upload firmware or use the serial monitor.

## Configuration and Reset
- On first boot, the radio starts the **ESP32-Radio-Setup** access point. Connect with password **radio1234**, then open `http://192.168.4.1`.
- To force setup mode later, hold **BOOT** while powering on or resetting the board. If the saved Wi-Fi network cannot be reached for 20 seconds, setup mode starts automatically.
- After connecting to Wi-Fi, open the radio's IP address (printed in the serial monitor) to change credentials, edit the list of up to 10 stream URLs, or select a station.
- **Reset configuration** erases the saved Wi-Fi and station settings and reboots into setup mode. Settings are stored in ESP32 non-volatile storage and survive power loss.

## Special Note on Flashing
If you encounter `No serial data received` errors during upload:
- Hold the **BOOT** button.
- Tap **RESET**.
- Release **BOOT**.
- Click **Upload**.
