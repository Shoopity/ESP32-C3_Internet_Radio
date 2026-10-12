#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceICYStream.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <RotaryEncoder.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>

//const char *DEFAULT_URL = "http://das-edge63-live365-dal03.cdnstream.com/a43564";
const char *DEFAULT_URL = "http://streaming.live365.com/a43564";
const char *SETUP_AP_SSID = "ESP32-Radio-Setup";
const char *SETUP_AP_PASSWORD = "radio1234";
constexpr uint8_t BOOT_BUTTON_PIN = 9;
constexpr uint8_t ENCODER_CLK_PIN = 4;
constexpr uint8_t ENCODER_DT_PIN = 5;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint8_t VOLUME_STEP = 5;
constexpr uint8_t VOLUME_MAX = 100;
constexpr char OTA_USERNAME[] = "radio";

// I2S Pins for NS4168
#define I2S_BCLK 1
#define I2S_LRC 2
#define I2S_DOUT 3

AudioGeneratorMP3 *mp3;
AudioFileSourceICYStream *file;
AudioFileSourceBuffer *buff;
AudioOutputI2S *out;
Preferences preferences;
WebServer server(80);
String wifiSsid;
String wifiPassword;
String stationList;
uint32_t currentStation = 0;
bool setupMode = false;
bool webServerConfigured = false;
bool otaConfigured = false;
bool otaAudioPaused = false;
bool otaUploadFailed = false;
bool otaUploadRejected = false;
bool otaUploadSucceeded = false;
uint8_t currentVolume = 25;
RotaryEncoder encoder(ENCODER_DT_PIN, ENCODER_CLK_PIN,
                      RotaryEncoder::LatchMode::FOUR3);

void MDCallback(void *cbData, const char *type, bool isUnicode,
                const char *string);
void StatusCallback(void *cbData, int code, const char *string);
void startSetupAccessPoint();
void configureArduinoOTA();
void handleEncoder();
void handleFirmwareUpload();
bool otaAvailable();
bool otaAuthorized();
void pauseAudioForOTA();
void resumeAudioAfterOTA();

void handleEncoder() {
  encoder.tick();
  int position = encoder.getPosition();
  if (position < 0) {
    encoder.setPosition(0);
    position = 0;
  } else if (position > VOLUME_MAX / VOLUME_STEP) {
    encoder.setPosition(VOLUME_MAX / VOLUME_STEP);
    position = VOLUME_MAX / VOLUME_STEP;
  }

  uint8_t newVolume = position * VOLUME_STEP;
  if (newVolume == currentVolume) return;

  currentVolume = newVolume;
  preferences.putUChar("volume", currentVolume);
  if (out != nullptr) {
    out->SetGain(static_cast<float>(currentVolume) / 50.0f);
  }
  Serial.printf("Volume: %u%%\n", currentVolume);
}

std::vector<String> getStations() {
  std::vector<String> stations;
  int start = 0;
  while (start < stationList.length() && stations.size() < 10) {
    int end = stationList.indexOf('\n', start);
    if (end < 0) end = stationList.length();
    String station = stationList.substring(start, end);
    station.trim();
    if (station.startsWith("http://") || station.startsWith("https://")) {
      stations.push_back(station);
    }
    start = end + 1;
  }
  return stations;
}

String htmlEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
    case '&': escaped += F("&amp;"); break;
    case '<': escaped += F("&lt;"); break;
    case '>': escaped += F("&gt;"); break;
    case '"': escaped += F("&quot;"); break;
    case '\'': escaped += F("&#39;"); break;
    default: escaped += value[i]; break;
    }
  }
  return escaped;
}

void startStream(uint32_t stationIndex) {
  std::vector<String> stations = getStations();
  if (stations.empty()) return;
  currentStation = stationIndex % stations.size();

  if (mp3 != nullptr) mp3->stop();
  delete buff;
  delete file;

  file = new AudioFileSourceICYStream(stations[currentStation].c_str());
  file->RegisterMetadataCB(MDCallback, NULL);
  file->RegisterStatusCB(StatusCallback, NULL);
  buff = new AudioFileSourceBuffer(file, 16384);
  buff->RegisterStatusCB(StatusCallback, NULL);
  if (mp3 == nullptr) mp3 = new AudioGeneratorMP3();

  Serial.printf("Starting stream: %s\n", stations[currentStation].c_str());
  if (!mp3->begin(buff, out)) {
    Serial.println("Error: Could not start MP3 generator");
  }
}

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Redirecting");
}

bool otaAvailable() {
  return !setupMode && WiFi.status() == WL_CONNECTED && !wifiPassword.isEmpty();
}

bool otaAuthorized() {
  return otaAvailable() && server.authenticate(OTA_USERNAME, wifiPassword.c_str());
}

void pauseAudioForOTA() {
  if (mp3 != nullptr && mp3->isRunning()) mp3->stop();
  delete buff;
  buff = nullptr;
  delete file;
  file = nullptr;
  otaAudioPaused = true;
}

void resumeAudioAfterOTA() {
  if (!otaAudioPaused) return;
  otaAudioPaused = false;
  startStream(currentStation);
}

void configureArduinoOTA() {
  if (otaConfigured || !otaAvailable()) return;

  ArduinoOTA.setHostname("esp32-radio");
  ArduinoOTA.setPassword(wifiPassword.c_str());
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.println("OTA started: " + type);
    pauseAudioForOTA();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA progress: %u%%\r", (progress * 100u) / total);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA complete. Restarting...");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("auth failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("begin failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("connect failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("receive failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("end failed");
    }
    resumeAudioAfterOTA();
  });

  ArduinoOTA.begin();
  otaConfigured = true;
  Serial.println("Arduino OTA ready. Update over Wi-Fi using the browser update page or port 3232.");
}

void handleFirmwareUpload() {
  HTTPUpload &upload = server.upload();

  switch (upload.status) {
    case UPLOAD_FILE_START:
      otaUploadFailed = false;
      otaUploadRejected = !otaAuthorized();
      otaUploadSucceeded = false;
      if (otaUploadRejected) return;
      if (!upload.filename.endsWith(".bin")) {
        otaUploadFailed = true;
        return;
      }
      Serial.printf("Firmware upload started: %s\n", upload.filename.c_str());
      pauseAudioForOTA();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
        otaUploadFailed = true;
        resumeAudioAfterOTA();
      }
      break;

    case UPLOAD_FILE_WRITE:
      if (!otaUploadRejected && !otaUploadFailed && upload.currentSize > 0) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
          Update.abort();
          otaUploadFailed = true;
          resumeAudioAfterOTA();
        }
      }
      break;

    case UPLOAD_FILE_END:
      if (!otaUploadRejected && !otaUploadFailed && Update.end(true)) {
        Serial.printf("Firmware upload complete: %u bytes\n", upload.totalSize);
        otaUploadSucceeded = true;
      } else {
        if (!otaUploadRejected && !otaUploadFailed) Update.printError(Serial);
        otaUploadFailed = true;
        resumeAudioAfterOTA();
      }
      break;

    case UPLOAD_FILE_ABORTED:
      if (!otaUploadRejected) Update.abort();
      otaUploadFailed = true;
      resumeAudioAfterOTA();
      Serial.println("Firmware upload aborted.");
      break;

    default:
      break;
  }
}

void configureWebServer() {
  if (webServerConfigured) return;

  server.on("/", HTTP_GET, []() {
    std::vector<String> stations = getStations();
    String page = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>ESP32 Radio</title><style>body{font:16px sans-serif;max-width:680px;margin:2rem auto;padding:0 1rem}"
                    "input,textarea,button{box-sizing:border-box;font:inherit;padding:.65rem;margin:.25rem 0;width:100%}"
                    "textarea{min-height:9rem}button{cursor:pointer}form{margin:1rem 0;padding:1rem 0;border-top:1px solid #ccc}"
                    ".station{display:flex;align-items:center;gap:1rem}.station form{flex:1}</style></head><body>"
                    "<h1>ESP32 Internet Radio</h1>");
    if (setupMode) {
      page += F("<p>Setup access point: <b>ESP32-Radio-Setup</b> &middot; password: <b>radio1234</b><br>"
                "Open <b>http://192.168.4.1</b> if this page does not appear automatically.</p>");
    }
    page += F("<h2>Stations</h2>");
    if (stations.empty()) page += F("<p>No valid station URLs saved.</p>");
    for (size_t i = 0; i < stations.size(); ++i) {
      page += F("<form class='station' method='post' action='/play'><span>");
      page += htmlEscape(stations[i]);
      page += F("</span><input type='hidden' name='station' value='");
      page += String(i);
      page += F("'><button type='submit'>");
      page += (i == currentStation && !setupMode) ? F("Playing") : F("Play");
      page += F("</button></form>");
    }
    if (otaAvailable()) {
      page += F("<h2>Firmware</h2><form method='get' action='/update'><button type='submit'>Upload firmware</button></form>");
    }
    page += F("<h2>Configuration</h2><form method='post' action='/save'>"
              "<label>Wi-Fi network<input name='ssid' maxlength='32' value='");
    page += htmlEscape(wifiSsid);
    page += F("' required></label><label>Wi-Fi password<input name='password' type='password' maxlength='64' value='");
    page += htmlEscape(wifiPassword);
    page += F("'></label><label>Stream URLs, one per line (up to 10)<textarea name='urls' required>");
    page += htmlEscape(stationList);
    page += F("</textarea></label><button type='submit'>Save and restart</button></form>"
              "<form method='post' action='/reset' onsubmit=\"return confirm('Erase saved Wi-Fi and station settings?')\">"
              "<button type='submit'>Reset configuration</button></form></body></html>");
    server.send(200, "text/html", page);
  });

  server.on("/update", HTTP_GET, []() {
    if (!otaAvailable()) {
      server.send(403, "text/plain", "OTA requires a saved Wi-Fi password and a connection to that network.");
      return;
    }
    if (!otaAuthorized()) {
      server.requestAuthentication();
      return;
    }

    String page = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>ESP32 Radio OTA</title><style>body{font:16px sans-serif;max-width:680px;margin:2rem auto;padding:0 1rem}"
                    "input,button{box-sizing:border-box;font:inherit;padding:.75rem;width:100%;margin:.5rem 0}"
                    "button{cursor:pointer}</style></head><body>"
                    "<h1>ESP32 Radio Firmware Update</h1>"
                    "<p>Upload a .bin firmware image. The device will reboot automatically after a successful update.</p>"
                    "<form method='post' action='/update' enctype='multipart/form-data'>"
                    "<input type='file' name='firmware' accept='.bin' required>"
                    "<button type='submit'>Upload and install</button></form>"
                    "<p><a href='/'>Back to radio</a></p></body></html>");
    server.send(200, "text/html", page);
  });

  server.on("/update", HTTP_POST, []() {
    if (!otaAvailable()) {
      server.send(403, "text/plain", "OTA requires a saved Wi-Fi password and a connection to that network.");
      return;
    }
    if (!otaAuthorized()) {
      server.requestAuthentication();
      return;
    }
    if (otaUploadRejected) {
      server.send(403, "text/plain", "Firmware update was not authorized.");
      return;
    }
    if (!otaUploadSucceeded) {
      Serial.println("Firmware upload failed.");
      server.send(500, "text/plain", "Firmware upload failed. Please try again.");
      resumeAudioAfterOTA();
      return;
    }

    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "Firmware update complete. Restarting...");
    delay(1000);
    ESP.restart();
  }, []() {
    handleFirmwareUpload();
  });

  server.on("/save", HTTP_POST, []() {
    String newSsid = server.arg("ssid");
    String newPassword = server.arg("password");
    String newStationList = server.arg("urls");
    newStationList.replace("\r", "");
    stationList = newStationList;
    std::vector<String> stations = getStations();
    if (newSsid.isEmpty() || stations.empty()) {
      server.send(400, "text/plain", "Enter a Wi-Fi network and at least one http:// or https:// stream URL.");
      return;
    }
    wifiSsid = newSsid;
    wifiPassword = newPassword;
    currentStation = 0;
    preferences.putString("ssid", wifiSsid);
    preferences.putString("password", wifiPassword);
    preferences.putString("stations", stationList);
    preferences.putUInt("current", currentStation);
    server.send(200, "text/html", "<p>Settings saved. Restarting...</p>");
    delay(500);
    ESP.restart();
  });

  server.on("/play", HTTP_POST, []() {
    if (!setupMode && server.hasArg("station")) {
      startStream(server.arg("station").toInt());
      preferences.putUInt("current", currentStation);
    }
    redirectHome();
  });

  server.on("/reset", HTTP_POST, []() {
    preferences.clear();
    server.send(200, "text/html", "<p>Configuration erased. Restarting into setup mode...</p>");
    delay(500);
    ESP.restart();
  });

  server.onNotFound([]() { redirectHome(); });
  server.begin();
  webServerConfigured = true;
}

void startSetupAccessPoint() {
  if (setupMode) {
    Serial.println("Setup access point is already active.");
    return;
  }

  if (mp3 != nullptr && mp3->isRunning()) mp3->stop();
  delete buff;
  buff = nullptr;
  delete file;
  file = nullptr;

  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD)) {
    Serial.println("Error: Could not start the setup access point.");
    return;
  }

  setupMode = true;
  Serial.printf("Setup AP started. Connect to %s (password: %s), then open http://%s\n",
                SETUP_AP_SSID, SETUP_AP_PASSWORD,
                WiFi.softAPIP().toString().c_str());
  configureWebServer();
}

void processSerialCommand(String command) {
  command.trim();
  command.toLowerCase();

  if (command == "ap" || command == "setup") {
    Serial.println("Switching to setup access point...");
    startSetupAccessPoint();
  } else if (command == "help") {
    Serial.println("Commands: ap (or setup) - disconnect from Wi-Fi and start setup AP; help - show commands");
  } else if (!command.isEmpty()) {
    Serial.printf("Unknown command: %s. Type help for commands.\n", command.c_str());
  }
}

void handleSerialCommands() {
  static String command;
  while (Serial.available() > 0) {
    char character = static_cast<char>(Serial.read());
    if (character == '\r') continue;
    if (character == '\n') {
      processSerialCommand(command);
      command = "";
    } else if (command.length() < 32) {
      command += character;
    }
  }
}

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
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);
  pinMode(ENCODER_DT_PIN, INPUT_PULLUP);
  delay(2000);
  Serial.println("\n\nESP32-C3 Internet Radio (ESP8266Audio) Starting...");
  Serial.println("Type 'ap' in the serial monitor to switch to setup access point; type 'help' for commands.");
  bool forceSetupMode = digitalRead(BOOT_BUTTON_PIN) == LOW;

  preferences.begin("radio", false);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  stationList = preferences.getString("stations", DEFAULT_URL);
  currentStation = preferences.getUInt("current", 0);
  currentVolume = preferences.getUChar("volume", 25);
  if (currentVolume > VOLUME_MAX) currentVolume = 25;
  encoder.setPosition(currentVolume / VOLUME_STEP);

  if (forceSetupMode || wifiSsid.isEmpty()) {
    startSetupAccessPoint();
    return;
  }

  Serial.printf("Connecting to %s ", wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  uint32_t connectionStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectionStart < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi connection failed; starting setup AP.");
    startSetupAccessPoint();
    return;
  }

  Serial.println("\nWiFi Connected!");
  Serial.printf("Open http://%s to configure the radio.\n", WiFi.localIP().toString().c_str());

  configureArduinoOTA();

  // Setup I2S Output
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(static_cast<float>(currentVolume) / 50.0f);

  configureWebServer();
  startStream(currentStation);
}

void loop() {
  handleSerialCommands();
  handleEncoder();
  server.handleClient();
  ArduinoOTA.handle();
  if (mp3 != nullptr && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      Serial.println("Stream stopped or finished");
    }
  } else if (!setupMode && mp3 != nullptr) {
    Serial.println("Stream error, retrying in 5s...");
    delay(5000);
    startStream(currentStation);
  }
}
