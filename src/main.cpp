#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceICYStream.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <Arduino.h>
#include <WiFi.h>

// WiFi Credentials - PLEASE UPDATE THESE
const char *ssid = "WOPR";
const char *password = "Wouldyoulike2playagame?";

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
  Serial.printf("METADATA(%s): %s\n", type, string);
  Serial.flush();
}

// Status callback
void StatusCallback(void *cbData, int code, const char *string) {
  (void)cbData;
  Serial.printf("STATUS(%d): %s\n", code, string);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\nESP32-C3 Internet Radio (ESP8266Audio) Starting...");

  // Connect to WiFi
  Serial.printf("Connecting to %s ", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");

  // Setup I2S Output
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(0.5); // Set initial volume (0.0 to 4.0)

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
    Serial.println("Stream error, retrying in 5s...");
    delay(5000);
    // Retry logic: stop and restart
    mp3->stop();
    if (!mp3->begin(buff, out)) {
      Serial.println("Error: Re-start failed");
    }
  }
}
