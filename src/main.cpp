#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceICYStream.h"
#include "AudioGeneratorAAC.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorNoise.h"
#include "AudioOutputI2S.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Preferences.h>
#include <RotaryEncoder.h>
#include <WiFiManager.h>
#include <Wire.h>

// I2S Pins for NS4168
#define I2S_BCLK 1
#define I2S_LRC 2
#define I2S_DOUT 3

// Rotary Encoder Pins
#define PIN_IN1 4
#define PIN_IN2 5

// Hardware BOOT button for factory reset
#define PIN_BOOT 9

// SSD1306 OLED Display (I2C)
#define OLED_SDA 6
#define OLED_SCL 7
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool display_ok = false;

// Cathedral radio tuner window display state (Portrait 64x128)
const int   DIAL_CX    = 32;   // Horizontal center
const int   DIAL_CY    = 180;  // Pivot below screen
const int   ARC_R      = 160;  // Radius for the wheel
const float STATION_ANGLES[5] = {60.0f, 75.0f, 90.0f, 105.0f, 120.0f};
const char *DIAL_LABELS[5]    = {"54", "70", "90", "110", "130"};
float view_angle = 90.0f;
float last_drawn_angle = -1.0f;
unsigned long display_update_timer = 0;

RotaryEncoder encoder(PIN_IN1, PIN_IN2, RotaryEncoder::LatchMode::TWO03);

AudioGenerator *decoder = NULL;
AudioGeneratorNoise *noise = NULL;
AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
AudioOutputI2S *out = NULL;

int current_volume = 10; // 0 to 100
bool is_playing = false;
bool is_tuning = false;
unsigned long reconnect_timer = 0;

// URL Storage
Preferences preferences;
char stream_urls[5][128] = {
    "http://das-edge63-live365-dal03.cdnstream.com/a43564", // Default Station 1
    "", "", "", "" // Slots 2-5 empty by default
};
int current_station_index = 0;
bool shouldSaveConfig = false;

// Global WiFiManager
WiFiManager wm;
WiFiManagerParameter *custom_url_params[5]; // Pointers to the text boxes

// Callback to trigger when WiFiManager portal saves
void saveConfigCallback() {
  Serial.println("Config saved in portal.");
  shouldSaveConfig = true;
}

// Interrupt routine for the encoder
IRAM_ATTR void checkPosition() { encoder.tick(); }

// Helper: convert wheel angle to screen x,y relative to the current view_angle
static void arcPt(float a_deg, float v_deg, int r, int &sx, int &sy) {
  float a_screen = 90.0f + (a_deg - v_deg);
  float rad = a_screen * (float)M_PI / 180.0f;
  sx = DIAL_CX + (int)(r * cosf(rad));
  sy = DIAL_CY - (int)(r * sinf(rad));
}

void drawDisplay() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- Draw the rotating scale ---
  float window_range = 30.0f; // degrees visible

  // Main arc line
  {
    int px, py, cx, cy;
    arcPt(view_angle - window_range, view_angle, ARC_R, px, py);
    for (float a = view_angle - window_range + 2; a <= view_angle + window_range; a += 2) {
      arcPt(a, view_angle, ARC_R, cx, cy);
      if (cx >= 0 && cx < 64 && cy >= 0 && cy < 128) {
        display.drawLine(px, py, cx, cy, SSD1306_WHITE);
      }
      px = cx; py = cy;
    }
  }

  // Ticks and labels
  display.setTextSize(1);
  for (float a = 40.0f; a <= 140.0f; a += 3.0f) {
    int station_idx = -1;
    for (int i = 0; i < 5; i++) {
      if (fabsf(STATION_ANGLES[i] - a) < 1.0f) {
        station_idx = i;
        break;
      }
    }

    int x1, y1, x2, y2;
    if (station_idx != -1) {
      arcPt(a, view_angle, ARC_R - 10, x1, y1);
      arcPt(a, view_angle, ARC_R + 10, x2, y2);
      
      int lx, ly;
      arcPt(a, view_angle, ARC_R - 22, lx, ly);
      
      if (lx > 4 && lx < 60 && ly > 4 && ly < 120) {
        display.setCursor(lx - 6, ly - 4);
        display.print(DIAL_LABELS[station_idx]);
      }
    } else {
      arcPt(a, view_angle, ARC_R - 4, x1, y1);
      arcPt(a, view_angle, ARC_R + 4, x2, y2);
    }

    if (x1 >= 0 && x1 < 64 && y1 >= 0 && y1 < 128 &&
        x2 >= 0 && x2 < 64 && y2 >= 0 && y2 < 128) {
      display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
    }
  }

  // --- Fixed Pointer (Needle) ---
  display.drawLine(32, 10, 32, 30, SSD1306_WHITE);
  display.fillTriangle(28, 10, 36, 10, 32, 18, SSD1306_WHITE);

  display.display();
  last_drawn_angle = view_angle;
}

// Callback for ICY metadata (log only — display no longer shows track info)
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

void turnOffRadio(bool keep_i2s_alive) {
  if (!is_playing && !is_tuning)
    return; // already fully off

  if (!keep_i2s_alive) {
    Serial.println("\n--- SOFTWARE OFF: Tearing down everything ---");
  } else {
    Serial.println(
        "\n--- TUNING MODE: Tearing down stream, keeping speaker alive ---");
  }

  // Stop the MP3 stream
  if (decoder) {
    decoder->stop();
    delete decoder;
    decoder = NULL;
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

  if (!keep_i2s_alive) {
    // Also stop noise generator and I2S output
    if (noise) {
      noise->stop();
      delete noise;
      noise = NULL;
    }
    if (out) {
      out->stop();
      delete out;
      out = NULL;
    }
    is_tuning = false;
  } else {
    // Start noise generator so the speaker produces static
    if (!noise) {
      noise = new AudioGeneratorNoise(
          0.10f); // 10% amplitude — audible but not harsh
      if (!noise->begin(NULL, out)) {
        Serial.println("Warning: Failed to start noise generator");
        delete noise;
        noise = NULL;
      } else {
        Serial.println("Noise generator started");
      }
    }
    is_tuning = true;
  }
  is_playing = false;
}

void turnOnRadio() {
  if (is_playing)
    return;
  Serial.println("\n--- SOFTWARE ON: Starting audio stream ---");

  // Find a valid station to play (skip empty ones)
  // For now, it just tries to play current_station_index.
  // If it's empty, it will drop to tuning mode.
  char *target_url = stream_urls[current_station_index];

  // CRITICAL FIX: We must delete the old I2S object before creating a new one
  // Otherwise the ESP32 throws "register I2S object to platform failed"
  if (out) {
    out->stop();
    delete out;
    out = NULL;
  }

  // Stop noise generator before starting the MP3 stream
  if (noise) {
    noise->stop();
    delete noise;
    noise = NULL;
  }

  if (!out) {
    out = new AudioOutputI2S();
    out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  }

  // Scale 0-100 to 0.0-2.0 gain
  float gain = (float)current_volume / 100.0 * 2.0;
  out->SetGain(gain);

  file = new AudioFileSourceICYStream(target_url);
  file->RegisterMetadataCB(MDCallback, NULL);
  file->RegisterStatusCB(StatusCallback, NULL);

  buff = new AudioFileSourceBuffer(file, 16384);
  buff->RegisterStatusCB(StatusCallback, NULL);

  // Basic stream format detection
  String urlStr = String(target_url);
  urlStr.toLowerCase();
  if (urlStr.indexOf("aac") > 0 || urlStr.indexOf("wzph") > 0) {
    Serial.println("Auto-detected AAC stream format.");
    decoder = new AudioGeneratorAAC();
  } else {
    decoder = new AudioGeneratorMP3();
  }

  Serial.printf("Starting stream: %s\n", target_url);
  if (strlen(target_url) == 0 || !decoder->begin(buff, out)) {
    Serial.println("Error: Could not start Decoder (or URL is empty)");
    turnOffRadio(current_volume > 0); // Drop into tuning mode if volume > 0
    reconnect_timer = millis();       // Start reconnect timer
  } else {
    is_playing = true;
    is_tuning = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\nESP32-C3 Internet Radio Starting...");

  // Init OLED display
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000); // Fast I2C for smoother animation
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    display_ok = true;
    display.setRotation(1); // Portrait mode
    display.clearDisplay();
    display.display();
    Serial.println("SSD1306 display OK");
  } else {
    Serial.println("WARNING: SSD1306 init failed. Check wiring.");
  }

  // Setup BOOT button input
  pinMode(PIN_BOOT, INPUT_PULLUP);

  // Setup Rotary Encoder
  attachInterrupt(digitalPinToInterrupt(PIN_IN1), checkPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_IN2), checkPosition, CHANGE);
  encoder.setPosition(current_volume);

  // Load custom URLs from Memory
  preferences.begin("radio", false);
  for (int i = 0; i < 5; i++) {
    String key = "url" + String(i);
    String saved_url = preferences.getString(key.c_str(), "");
    if (saved_url.length() > 0) {
      strncpy(stream_urls[i], saved_url.c_str(), sizeof(stream_urls[i]));
    }
  }

  // Captive Portal Setup
  Serial.println("Starting WiFiManager...");
  wm.setSaveConfigCallback(saveConfigCallback);

  // Enable the "Setup" menu button on the main page
  std::vector<const char *> menu = {"wifi", "param", "info", "exit"};
  wm.setMenu(menu);

  // Dynamically generate the 5 text boxes
  for (int i = 0; i < 5; i++) {
    char *id = (char *)malloc(16);
    snprintf(id, 16, "url%d", i);
    char *label = (char *)malloc(32);
    snprintf(label, 32, "Station %d URL", i + 1);

    custom_url_params[i] =
        new WiFiManagerParameter(id, label, stream_urls[i], 128);
    wm.addParameter(custom_url_params[i]);
  }

  bool res = wm.autoConnect("ESP32_Radio_Config");
  if (!res) {
    Serial.println("Failed to connect or hit timeout");
  } else {
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());

    // Save URLs if they were changed in the portal
    if (shouldSaveConfig) {
      for (int i = 0; i < 5; i++) {
        strncpy(stream_urls[i], custom_url_params[i]->getValue(), 128);
        String key = "url" + String(i);
        preferences.putString(key.c_str(), stream_urls[i]);
      }
      Serial.println("Saved 5 Station URLs to memory.");
    }
  }

  // Start the radio if volume > 0
  if (current_volume > 0) {
    turnOnRadio();
  }
}

void loop() {
  // 1. Handle Hardware BOOT Button (Wipe Config)
  static bool boot_pressed = false;
  static unsigned long boot_press_start = 0;
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!boot_pressed) {
      boot_pressed = true;
      boot_press_start = millis();
    } else if (millis() - boot_press_start > 3000) {
      Serial.println(
          "\n[!] BOOT button held. Wiping WiFi and Station config...");
      turnOffRadio(false);
      wm.resetSettings();
      preferences.clear();
      delay(1000);
      ESP.restart();
    }
  } else {
    boot_pressed = false;
  }

  // 2. Handle Encoder Input
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

    if (current_volume == 0) {
      turnOffRadio(false); // True off
    } else if (current_volume > 0 && !is_playing && !is_tuning) {
      // Waking up from true off
      turnOnRadio();
    } else if (out) {
      // Just adjust the volume dynamically
      float gain = (float)current_volume / 100.0 * 2.0;
      out->SetGain(gain);
    }
  }

  // 3. Handle Audio State Machine
  if (is_playing) {
    if (decoder->isRunning()) {
      if (!decoder->loop()) {
        decoder->stop();
        Serial.println("Stream stopped or finished");
        turnOffRadio(current_volume >
                     0); // Keep I2S alive if volume > 0 for static
        reconnect_timer = millis();
      }
    }
  } else if (is_tuning) {
    // 4. Play Analog Static via proper AudioGenerator pipeline
    if (noise && noise->isRunning()) {
      noise->loop();
    }

    // 5. Non-Blocking Reconnect
    if (millis() - reconnect_timer > 5000) {
      Serial.println("Attempting to reconnect...");

      static int failed_retries = 0;
      // Check WiFi
      if (WiFi.status() != WL_CONNECTED || failed_retries >= 3) {
        if (failed_retries >= 3) {
          Serial.println("Multiple retries failed. Forcing WiFi reset...");
        } else {
          Serial.println("WiFi dropped, reconnecting...");
        }
        WiFi.disconnect();
        delay(1000); // Small block to let WiFi modem reset, static will pause
                     // briefly
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
      turnOnRadio();

      if (!is_playing) {
        failed_retries++;
        // turnOnRadio already dropped us back into is_tuning = true
        reconnect_timer = millis(); // Reset timer for the next 5s wait
      } else {
        failed_retries = 0;
      }
    }
  }

  // --- Animation and Display Updates ---
  bool needs_draw = false;
  if (is_tuning) {
    // Rapid oscillation when lost/tuning
    float t = (float)millis() / 500.0f;
    view_angle = 90.0f + 60.0f * sinf(t);
    needs_draw = true;
  } else if (is_playing) {
    float target = STATION_ANGLES[current_station_index];
    if (fabsf(target - view_angle) > 0.1f) {
      view_angle += (target - view_angle) * 0.15f;
      needs_draw = true;
    }
  }

  if (needs_draw) {
    unsigned long disp_ms = is_tuning ? 50UL : 100UL;
    if (millis() - display_update_timer > disp_ms) {
      display_update_timer = millis();
      drawDisplay();
    }
  }
}
