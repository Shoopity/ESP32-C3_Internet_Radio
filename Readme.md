# ESP32-C3 Internet Radio

This project is a small Wi-Fi internet radio built around an ESP32-C3 Super Mini. It connects to a Wi-Fi network, plays MP3 streams from configured URLs, and exposes a simple browser-based configuration page to save Wi-Fi credentials and station list.

The firmware is implemented in [src/main.cpp](src/main.cpp) and uses the ESP8266Audio library to decode internet radio streams and send audio to an I2S amplifier.

## Project overview

The radio performs the following tasks:

- Starts in normal mode if a saved Wi-Fi network is available
- Connects to the configured Wi-Fi access point and opens the stream list
- Downloads live MP3 or ICY streams over HTTP
- Decodes the audio stream using `AudioGeneratorMP3`
- Sends the decoded audio to an I2S output
- Falls back to a setup access point when no network is configured or the Wi-Fi connection fails
- Provides a lightweight web interface to edit the station list, Wi-Fi credentials, or reset settings

## Hardware used

- ESP32-C3 Super Mini
- NS4168 I2S mono audio amplifier
- USB-C power source for the ESP32
- Speaker connected to the amplifier output

## Pinout and wiring

The current firmware defines the I2S audio pins explicitly and uses them when setting up the audio output:

- `I2S_BCLK = 1`
- `I2S_LRC = 2`
- `I2S_DOUT = 3`

The encoder support was also added for volume control:

- `ENCODER_CLK_PIN = 4`
- `ENCODER_DT_PIN = 5`

These are connected as follows:

| Device | Signal | ESP32-C3 pin | Notes |
|---|---|---:|---|
| NS4168 amplifier | BCLK | GPIO 1 | I2S bit clock |
| NS4168 amplifier | LRC | GPIO 2 | I2S left/right clock |
| NS4168 amplifier | DIN | GPIO 3 | I2S audio data input |
| KY040 encoder | CLK | GPIO 4 | Volume knob clock signal |
| KY040 encoder | DT | GPIO 5 | Volume knob data signal |
| NS4168 amplifier | V (5V) | 5V | Power rail |
| NS4168 amplifier | G (GND) | GND | Common ground |
| KY040 encoder | GND | GND | Encoder ground |
| KY040 encoder | VCC | 3V3 | Power from the ESP32-C3 3.3V rail |

The firmware also uses the BOOT button on the ESP32-C3 for forced setup mode:

| Device | Signal | ESP32-C3 pin | Notes |
|---|---|---:|---|
| BOOT button | BOOT | GPIO 9 | Pressed during startup to force setup mode |

> The code sets `pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP)` and checks `digitalRead(BOOT_BUTTON_PIN) == LOW` during setup to determine whether to start the setup access point. The encoder uses `INPUT_PULLUP` on GPIO 4 and GPIO 5 for reliable readout.

## Audio path

The firmware configures the I2S amplifier with:

```cpp
out = new AudioOutputI2S();
out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
out->SetGain(static_cast<float>(currentVolume) / 50.0f);
```

This means the amplified audio is sent from the ESP32-C3 to the NS4168 module using the standard I2S interface:

- Bit clock: GPIO 1
- Word select / LR clock: GPIO 2
- Serial audio data: GPIO 3

At the same time, the radio reads the KY040 encoder state in `handleEncoder()` and adjusts the volume in 5% steps from 0% to 100%. The setting is saved in ESP32 non-volatile preferences and restored on reboot. The default 25% volume maps to an audio gain of 0.5.

## Wi-Fi and setup behavior

The radio uses the ESP32's non-volatile preferences storage to remember saved network settings and station URLs.

### Default station

The project starts with this default URL when no station list is stored:

```cpp
const char *DEFAULT_URL = "http://streaming.live365.com/a43564";
```

### Setup access point

If the device cannot find saved Wi-Fi credentials or the connection fails, the firmware starts a setup access point named:

- SSID: `ESP32-Radio-Setup`
- Password: `radio1234`

The web server is then available at:

- `http://192.168.4.1`

The configuration page allows the user to:

- Enter a Wi-Fi SSID and password
- Add up to 10 station URLs, one per line
- Save settings and restart
- Reset saved values and return to setup mode

### Serial commands

The firmware accepts simple commands over the serial monitor:

- `ap` or `setup` — switch to setup access point
- `help` — display command list

These commands are processed in `processSerialCommand()` and can be used when connecting through a serial monitor at 115200 baud.

## Software stack

The project is built using PlatformIO.

### Platform configuration

From [platformio.ini](platformio.ini):

- Platform: `espressif32@~6.7.0`
- Board: `esp32-c3-devkitm-1`
- Framework: Arduino
- Monitor speed: `115200`
- Upload speed: `115200`
- Libraries: `earlephilhower/ESP8266Audio@1.9.7`, `mathertel/RotaryEncoder@^1.5.3`

### Main libraries used

- `ESP8266Audio` for MP3 playback and stream handling
- `WiFi` for network connectivity
- `WebServer` for the local configuration page
- `Preferences` for persistent storage
- `RotaryEncoder` for KY040 quadrature decoding
- `AudioGeneratorMP3` to decode the stream
- `AudioOutputI2S` to send audio to the amplifier

## Functional flow

1. Board boots and reads stored Wi-Fi and station settings from flash
2. If setup is forced or no Wi-Fi settings exist, the radio starts the setup AP
3. Otherwise, the ESP32 connects to the saved Wi-Fi network
4. The firmware loads the station list and begins streaming the selected URL
5. A browser can open the radio's local IP to manage network and station settings
6. The system can restart into setup mode if the Wi-Fi connection is unavailable or the configuration is reset

## Notes

- The board uses `GPIO 9` as a setup button input, so the BOOT button is intentionally reserved for startup configuration logic.
- The project is designed for a single-channel mono amplifier output; it is not a stereo design.
- The existing default station in the code is an internet radio stream and can be replaced by custom stations in the web interface.

## Example connection summary

```text
ESP32-C3 Super Mini
  5V  -----------------> NS4168 V (5V)
  GND -----------------> NS4168 G (GND)
  GPIO 1 --------------> NS4168 BCLK
  GPIO 2 --------------> NS4168 LRC
  GPIO 3 --------------> NS4168 DIN
  GPIO 4 --------------> KY040 CLK
  GPIO 5 --------------> KY040 DT
  GPIO 9 --------------> BOOT button input (setup mode)

KY040 encoder
  VCC -----------------> 3V3
  GND -----------------> GND
  CLK -----------------> GPIO 4
  DT -----------------> GPIO 5
```

This README is intentionally kept separate from the existing repository README for comparison and documentation review.
