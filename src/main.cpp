#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceICYStream.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <Arduino.h>
#include <WiFiManager.h>
#include <RotaryEncoder.h>

// Radio Stream URL (Using HTTP for compatibility)
const char *url = "http://das-edge63-live365-dal03.cdnstream.com/a43564";

// I2S Pins for NS4168
#define I2S_BCLK 1
#define I2S_LRC 2
#define I2S_DOUT 3

// Rotary Encoder Pins
#define PIN_IN1 4
#define PIN_IN2 5

RotaryEncoder encoder(PIN_IN1, PIN_IN2, RotaryEncoder::LatchMode::TWO03);

AudioGeneratorMP3 *mp3 = NULL;
AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
AudioOutputI2S *out = NULL;

int current_volume = 10; // 0 to 100
bool is_playing = false;

// Interrupt routine for the encoder
IRAM_ATTR void checkPosition() {
  encoder.tick();
}

// Callback for ICY metadata
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string) {
  (void)cbData;
  Serial.printf("METADATA(%s): %s\r\n", type, string);
  Serial.flush();
}

// Status callback
void StatusCallback(void *cbData, int code, const char *string) {
  (void)cbData;
  Serial.printf("STATUS(%d): %s\r\n", code, string);
  Serial.flush();
}

void turnOffRadio() {
  if (!is_playing) return;
  Serial.println("\n--- SOFTWARE OFF: Tearing down audio stream ---");
  if (mp3) { mp3->stop(); delete mp3; mp3 = NULL; }
  if (buff) { buff->close(); delete buff; buff = NULL; }
  if (file) { file->close(); delete file; file = NULL; }
  if (out) { out->stop(); delete out; out = NULL; }
  is_playing = false;
}

void turnOnRadio() {
  if (is_playing) return;
  Serial.println("\n--- SOFTWARE ON: Starting audio stream ---");
  
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  
  // Scale 0-100 to 0.0-2.0 gain
  float gain = (float)current_volume / 100.0 * 2.0; 
  out->SetGain(gain);

  file = new AudioFileSourceICYStream(url);
  file->RegisterMetadataCB(MDCallback, NULL);
  file->RegisterStatusCB(StatusCallback, NULL);
  
  buff = new AudioFileSourceBuffer(file, 16384);
  buff->RegisterStatusCB(StatusCallback, NULL);
  
  mp3 = new AudioGeneratorMP3();

  Serial.printf("Starting stream: %s\n", url);
  if (!mp3->begin(buff, out)) {
    Serial.println("Error: Could not start MP3 generator");
  } else {
    is_playing = true;
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\nESP32-C3 Internet Radio Starting...");

  // Setup Rotary Encoder
  attachInterrupt(digitalPinToInterrupt(PIN_IN1), checkPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_IN2), checkPosition, CHANGE);
  encoder.setPosition(current_volume);

  // Captive Portal Setup
  Serial.println("Starting WiFiManager...");
  WiFiManager wm;
  // wm.resetSettings(); // Wipes the saved WiFi memory on boot

  bool res = wm.autoConnect("ESP32_Radio_Config");
  if (!res) {
    Serial.println("Failed to connect or hit timeout");
  } else {
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
  }

  // Start the radio if volume > 0
  if (current_volume > 0) {
    turnOnRadio();
  }
}

void loop() {
  // 1. Handle Encoder Input
  encoder.tick(); // Backup check
  int newPos = encoder.getPosition();
  
  // Bound the volume between 0 and 100
  if (newPos < 0) {
    encoder.setPosition(0);
    newPos = 0;
  } else if (newPos > 100) {
    encoder.setPosition(100);
    newPos = 100;
  }

  if (newPos != current_volume) {
    current_volume = newPos;
    Serial.printf("Volume: %d%%\n", current_volume);
    
    if (current_volume == 0 && is_playing) {
      turnOffRadio();
    } else if (current_volume > 0 && !is_playing) {
      turnOnRadio();
    } else if (is_playing && out) {
      float gain = (float)current_volume / 100.0 * 2.0;
      out->SetGain(gain);
    }
  }

  // 2. Handle Audio Stream
  if (is_playing) {
    if (mp3->isRunning()) {
      if (!mp3->loop()) {
        mp3->stop();
        Serial.println("Stream stopped or finished");
        is_playing = false;
      }
    } else {
      // Reconnect logic
      static int failed_retries = 0;
      Serial.println("Stream error, retrying in 5s...");
      delay(5000);

      // Check WiFi
      if (WiFi.status() != WL_CONNECTED || failed_retries >= 3) {
        if (failed_retries >= 3) {
          Serial.println("Multiple retries failed. Forcing WiFi reset...");
        } else {
          Serial.println("WiFi dropped, reconnecting...");
        }
        WiFi.disconnect();
        delay(1000);
        WiFi.begin();
        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 20) {
          delay(500);
          Serial.print(".");
          retries++;
        }
        Serial.println();
        failed_retries = 0;
      }

      // Rebuild stream
      is_playing = true; // force turnOffRadio to execute
      turnOffRadio(); 
      turnOnRadio();  

      if (!is_playing) {
        failed_retries++;
      } else {
        failed_retries = 0;
      }
    }
  }
}
