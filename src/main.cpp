#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceICYStream.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>

const char *DEFAULT_URL = "http://das-edge63-live365-dal03.cdnstream.com/a43564";
const char *SETUP_AP_SSID = "ESP32-Radio-Setup";
const char *SETUP_AP_PASSWORD = "radio1234";
constexpr uint8_t BOOT_BUTTON_PIN = 9;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

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

void MDCallback(void *cbData, const char *type, bool isUnicode,
                const char *string);
void StatusCallback(void *cbData, int code, const char *string);

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

void configureWebServer() {
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
}

void startSetupAccessPoint() {
  setupMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
  Serial.printf("Setup AP started. Connect to %s (password: %s), then open http://192.168.4.1\n",
                SETUP_AP_SSID, SETUP_AP_PASSWORD);
  configureWebServer();
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
  bool forceSetupMode = digitalRead(BOOT_BUTTON_PIN) == LOW;
  delay(2000);
  Serial.println("\n\nESP32-C3 Internet Radio (ESP8266Audio) Starting...");

  preferences.begin("radio", false);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  stationList = preferences.getString("stations", DEFAULT_URL);
  currentStation = preferences.getUInt("current", 0);

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

  // Setup I2S Output
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(0.5); // Set initial volume (0.0 to 4.0)

  configureWebServer();
  startStream(currentStation);
}

void loop() {
  server.handleClient();
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
