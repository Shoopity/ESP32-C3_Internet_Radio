#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceICYStream.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <Arduino.h>
#include <WiFiManager.h>

// Radio Stream URL (Using HTTP for compatibility)
const char *url = "http://das-edge63-live365-dal03.cdnstream.com/a43564";

// I2S Pins for NS4168
#define I2S_BCLK 1
#define I2S_LRC 2
#define I2S_DOUT 3

AudioGeneratorMP3 *mp3;
AudioFileSourceICYStream *file;
AudioFileSourceBuffer *buff;
AudioOutputI2S *out;

// Callback for ICY metadata
void MDCallback(void *cbData, const char *type, bool isUnicode,
                const char *string) {
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

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\nESP32-C3 Internet Radio (ESP8266Audio) Starting...");

  // Captive Portal Setup
  Serial.println("Starting WiFiManager...");
  WiFiManager wm;
  // wm.resetSettings(); // Wipes the saved WiFi memory on boot

  // If it can't connect, it will create an AP named "ESP32_Radio_Config" with
  // no password
  bool res = wm.autoConnect("ESP32_Radio_Config");

  if (!res) {
    Serial.println("Failed to connect or hit timeout");
    // ESP.restart();
  } else {
    // if you get here you have connected to the WiFi
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
  }

  // Setup I2S Output
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(0.05); // Set initial volume (0.0 to 4.0)

  // Setup the stream source
  file = new AudioFileSourceICYStream(url);
  file->RegisterMetadataCB(MDCallback, NULL);
  file->RegisterStatusCB(StatusCallback, NULL);

  // Buffer to smooth out network jitter (16KB)
  buff = new AudioFileSourceBuffer(file, 16384);
  buff->RegisterStatusCB(StatusCallback, NULL);

  mp3 = new AudioGeneratorMP3();

  Serial.printf("Starting stream: %s\n", url);
  if (!mp3->begin(buff, out)) {
    Serial.println("Error: Could not start MP3 generator");
  }
}

void loop() {
  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      Serial.println("Stream stopped or finished");
    }
  } else {
    static int failed_retries = 0;
    Serial.println("Stream error, retrying in 5s...");
    delay(5000);

    // Check WiFi connection or force reconnect if we've failed too many times
    if (WiFi.status() != WL_CONNECTED || failed_retries >= 3) {
      if (failed_retries >= 3) {
        Serial.println("Multiple retries failed (possible DNS issue). Forcing "
                       "WiFi reset...");
      } else {
        Serial.println("WiFi dropped, reconnecting...");
      }

      WiFi.disconnect();
      delay(1000);
      WiFi.begin(); // Using no arguments automatically uses the credentials
                    // stored by WiFiManager
      int retries = 0;
      while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
      }
      Serial.println();
      failed_retries = 0; // Reset counter after WiFi bounce
    }

    // Teardown the old broken stream objects (MUST do mp3 first to prevent heap
    // corruption)
    if (mp3) {
      mp3->stop();
      delete mp3;
      mp3 = NULL;
    }
    if (buff) {
      buff->close();
      delete buff;
      buff = NULL;
    }
    if (file) {
      file->close();
      delete file;
      file = NULL;
    }
    if (out) {
      out->stop();
      delete out;
      out = NULL;
    }

    // Recreate the stream objects and output
    out = new AudioOutputI2S();
    out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    out->SetGain(0.051); // Keep the user's volume setting

    file = new AudioFileSourceICYStream(url);
    file->RegisterMetadataCB(MDCallback, NULL);
    file->RegisterStatusCB(StatusCallback, NULL);
    buff = new AudioFileSourceBuffer(file, 16384);
    buff->RegisterStatusCB(StatusCallback, NULL);
    mp3 = new AudioGeneratorMP3();

    // Try to start again
    if (!mp3->begin(buff, out)) {
      Serial.println("Error: Re-start failed");
      failed_retries++;
    } else {
      failed_retries = 0; // Success!
    }
  }
}
