#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceICYStream.h"
#include "AudioGeneratorAAC.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorNoise.h"
#include "AudioOutputI2S.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Fonts/FreeSansBold9pt7b.h>
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
const int DIAL_CX = 32;  // Horizontal center
const int DIAL_CY = 180; // Pivot below screen
const int ARC_R = 160;   // Radius for the wheel
const float STATION_ANGLES[5] = {40.0f, 65.0f, 90.0f, 115.0f, 140.0f};
const char *DIAL_LABELS_OUT[6] = {"108", "103", "100", "96", "92", "88"};
const char *DIAL_LABELS_IN[8] = {"120", "110", "100", "90",
                                 "80",  "70",  "60",  "54"};
const int VOLUME_STEPS[21] = {
    0, 1, 2, 4, 6, 9, 13, 18, 24, 32, 42, 
    53, 64, 74, 82, 88, 93, 96, 98, 99, 100
};
float view_angle = 90.0f;
float view_phase = 0.0f; // Persistent phase for smooth oscillation
float last_drawn_angle = -1.0f;
unsigned long display_update_timer = 0;

// #define PIN_SW 10 // Encoder Switch pin (Optional: uncomment if wired)
int last_volume_before_mute = 10;
bool is_muted = false;

// Custom subclass to fix a bug in the ESP8266Audio library where the AudioGeneratorMP3
// constructor leaves several internal decoding pointers uninitialized. If begin() fails
// before allocation, deleting the generator triggers a double-free crash.
class SafeAudioGeneratorMP3 : public AudioGeneratorMP3 {
public:
  SafeAudioGeneratorMP3() : AudioGeneratorMP3() {
    buff = nullptr;
    stream = nullptr;
    frame = nullptr;
    synth = nullptr;
  }
};

RotaryEncoder encoder(PIN_IN2, PIN_IN1, RotaryEncoder::LatchMode::FOUR3);

AudioGenerator *decoder = NULL;
AudioGeneratorNoise *noise = NULL;
AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
AudioOutputI2S *out = NULL;

int current_volume = 10; // Default startup volume (0 to 100)
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
  if (!display_ok)
    return;

  // Invert colors: White background, Black text
  display.clearDisplay();
  display.fillScreen(SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextWrap(
      false); // CRITICAL: Prevents numbers from 'wrapping' to the other side

  // Use a smooth, proportional font instead of blocky pixels
  display.setFont(&FreeSansBold9pt7b);
  float window_range = 35.0f;

  // --- Outer Row (6 Labels) ---
  for (int i = 0; i < 6; i++) {
    float a = 50.0f + (i * 16.0f);
    float dist = fabsf(a - view_angle);
    if (dist > window_range)
      continue;

    int lx, ly;
    arcPt(a, view_angle, ARC_R - 10, lx, ly);
    if (lx > 0 && lx < 64 && ly > 0 && ly < 128) {
      // NOTE: Custom fonts use baseline for Y, so we add 4 to center roughly
      display.setCursor(lx - 12, ly + 4);
      display.print(DIAL_LABELS_OUT[i]);
    }
  }

  // --- Inner Row (8 Labels) ---
  for (int i = 0; i < 8; i++) {
    float a = 35.0f + (i * 15.0f);
    float dist = fabsf(a - view_angle);
    if (dist > window_range)
      continue;

    int lx, ly;
    arcPt(a, view_angle, ARC_R - 40, lx, ly);
    if (lx > 0 && lx < 64 && ly > 0 && ly < 128) {
      display.setCursor(lx - 12, ly + 4);
      display.print(DIAL_LABELS_IN[i]);
    }
  }

  // --- Ticks ---
  // Draw small decorative ticks every 4 degrees to fill the space between
  // numbers
  for (float a = 25.0f; a <= 155.0f; a += 4.0f) {
    float dist = fabsf(a - view_angle);
    if (dist > window_range)
      continue;

    int x1, y1, x2, y2;
    // Outer ticks
    arcPt(a, view_angle, ARC_R - 3, x1, y1);
    arcPt(a, view_angle, ARC_R - 8, x2, y2);
    if (x1 > 0 && x1 < 64)
      display.drawLine(x1, y1, x2, y2, SSD1306_BLACK);

    // Inner ticks
    arcPt(a, view_angle, ARC_R - 32, x1, y1);
    arcPt(a, view_angle, ARC_R - 37, x2, y2);
    if (x1 > 0 && x1 < 64)
      display.drawLine(x1, y1, x2, y2, SSD1306_BLACK);
  }

  // --- Fixed Needle (Thin Line all the way down) ---
  // display.drawLine(32, 0, 32, 127, SSD1306_BLACK);

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

void cleanupAudio(bool includeI2S) {
  // 1. Stop and delete generators first (they depend on the buffers/output)
  if (decoder) {
    if (decoder->isRunning())
      decoder->stop();
    delete decoder;
    decoder = NULL;
  }
  if (noise) {
    if (noise->isRunning())
      noise->stop();
    delete noise;
    noise = NULL;
  }

  // 2. Stop and delete buffers/sources
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

  // 3. Stop and delete I2S output
  // We've found that on ESP32-C3, it's safer to fully release the driver
  // but we must give it a moment to 'breathe' before re-installing.
  if (out) {
    out->stop();
    if (includeI2S) {
      delete out;
      out = NULL;
      delay(50); // Critical: give hardware time to release I2S peripheral
    }
  }
}

void turnOffRadio(bool keep_i2s_alive) {
  if (!is_playing && !is_tuning)
    return;

  if (!keep_i2s_alive) {
    Serial.println("\r\n--- SOFTWARE OFF: Tearing down everything ---");
    cleanupAudio(true); // Full wipe
    is_tuning = false;
  } else {
    Serial.println(
        "\r\n--- TUNING MODE: Tearing down stream, keeping speaker alive ---");
    cleanupAudio(false); // Stop stream, keep I2S

    // Start noise generator so the speaker produces static
    if (!noise && out) {
      noise = new AudioGeneratorNoise(0.10f);
      // If 'out' was already running, begin() might still trigger a warning,
      // but it's much less likely to crash if we haven't uninstalled the
      // driver.
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
  Serial.println("\r\n--- SOFTWARE ON: Starting audio stream ---");

  char *target_url = stream_urls[current_station_index];
  if (strlen(target_url) == 0) {
    Serial.println("Error: Station URL is empty.");
    turnOffRadio(current_volume > 0);
    return;
  }

  // To prevent "I2S register failed", we do a full clean including I2S,
  // then recreate it fresh with the delay in cleanupAudio().
  cleanupAudio(true);

  if (!out) {
    out = new AudioOutputI2S();
    out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  }

  // Set gain
  float gain = (float)current_volume / 100.0 * 2.0;
  out->SetGain(gain);

  // Setup stream pipeline
  file = new AudioFileSourceICYStream(target_url);
  file->RegisterMetadataCB(MDCallback, NULL);
  file->RegisterStatusCB(StatusCallback, NULL);

  buff = new AudioFileSourceBuffer(file, 16384);
  buff->RegisterStatusCB(StatusCallback, NULL);

  // Auto-detect format
  String urlStr = String(target_url);
  urlStr.toLowerCase();
  if (urlStr.indexOf("aac") >= 0 || urlStr.indexOf("wzph") >= 0) {
    decoder = new AudioGeneratorAAC();
  } else {
    decoder = new SafeAudioGeneratorMP3();
  }

  Serial.printf("Starting stream: %s\r\n", target_url);
  if (!decoder->begin(buff, out)) {
    Serial.println("Error: Could not start Decoder");
    cleanupAudio(false);              // Clean up the failed attempt
    turnOffRadio(current_volume > 0); // Re-trigger tuning mode
    reconnect_timer = millis();
  } else {
    is_playing = true;
    is_tuning = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\r\nESP32-C3 Internet Radio Starting...");

  // Init OLED display
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000); // Fast I2C for smoother animation
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    display_ok = true;
    display.setRotation(1); // Portrait mode
    display.setTextWrap(false);
    display.clearDisplay();
    display.display();
    Serial.println("SSD1306 display OK");
  } else {
    Serial.println("WARNING: SSD1306 init failed. Check wiring.");
  }

  // Setup BOOT button input
  pinMode(PIN_BOOT, INPUT_PULLUP);

  // Setup Encoder SW pin (Optional: uncomment if wired)
  // pinMode(PIN_SW, INPUT_PULLUP);

  // Setup Rotary Encoder
  attachInterrupt(digitalPinToInterrupt(PIN_IN1), checkPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_IN2), checkPosition, CHANGE);
  encoder.setPosition(current_volume / 5); // Initialize in steps of 5% (0 to 20 clicks)

  // Load custom URLs and volume from Memory
  preferences.begin("radio", false);
  current_volume = preferences.getInt("volume", 10); // Load saved volume, default to 10
  if (current_volume < 0 || current_volume > 100) current_volume = 10;
  
  // Find the closest step in the lookup table to set the encoder starting position
  int best_step = 2; // Default to a low step
  int min_diff = 999;
  for (int i = 0; i <= 20; i++) {
    int diff = abs(VOLUME_STEPS[i] - current_volume);
    if (diff < min_diff) {
      min_diff = diff;
      best_step = i;
    }
  }
  encoder.setPosition(best_step);
  current_volume = VOLUME_STEPS[best_step]; // Snap to exact curve value

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
    Serial.println("\r\nWiFi Connected! IP: " + WiFi.localIP().toString());

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
    // Start at a random visual position for the aesthetic
    view_phase = (float)random(0, 628) / 100.0f;
    view_angle = 90.0f + 60.0f * sinf(view_phase);
    if (display_ok)
      drawDisplay(); // Show it immediately on boot
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
          "\r\n[!] BOOT button held. Wiping WiFi and Station config...");
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
  int clickPos = encoder.getPosition();
 
  // Bound the clicks between 0 and 20 (representing 0% to 100% in 5% steps)
  if (clickPos < 0) {
    encoder.setPosition(0);
    clickPos = 0;
  } else if (clickPos > 20) {
    encoder.setPosition(20);
    clickPos = 20;
  }
 
  int newPos = VOLUME_STEPS[clickPos];
 
  if (newPos != current_volume) {
    // If we adjust the knob, we unmute automatically
    if (is_muted && newPos > 0) {
      is_muted = false;
    }
    
    current_volume = newPos;
    Serial.printf("Volume: %d%%\r\n", current_volume);

    // Persist volume to memory so it is remembered on boot
    preferences.putInt("volume", current_volume);

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

  // 2b. Handle Optional Encoder Switch Mute (Optional: uncomment if wired)
  /*
  static bool sw_pressed = false;
  static unsigned long last_sw_time = 0;
  if (digitalRead(PIN_SW) == LOW) {
    if (!sw_pressed && (millis() - last_sw_time > 200)) { // 200ms debounce
      sw_pressed = true;
      last_sw_time = millis();
      if (is_muted) {
        is_muted = false;
        current_volume = last_volume_before_mute;
        Serial.printf("Unmuted! Volume: %d%%\r\n", current_volume);
        
        // Restore encoder click position
        int best_step = 2;
        int min_diff = 999;
        for (int i = 0; i <= 20; i++) {
          int diff = abs(VOLUME_STEPS[i] - current_volume);
          if (diff < min_diff) {
            min_diff = diff;
            best_step = i;
          }
        }
        encoder.setPosition(best_step);
        
        if (!is_playing && !is_tuning && current_volume > 0) {
          turnOnRadio();
        } else if (out) {
          float gain = (float)current_volume / 100.0 * 2.0;
          out->SetGain(gain);
        }
      } else {
        is_muted = true;
        last_volume_before_mute = current_volume;
        current_volume = 0;
        Serial.println("Muted!");
        
        encoder.setPosition(0);
        turnOffRadio(false); // Tear down stream completely
      }
    }
  } else {
    sw_pressed = false;
  }
  */

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
    static bool reconnecting = false;
    static unsigned long last_wifi_check = 0;

    if (WiFi.status() != WL_CONNECTED) {
      if (!reconnecting) {
        Serial.println("WiFi disconnected! Starting non-blocking reconnect...");
        WiFi.disconnect();
        WiFi.begin();
        reconnecting = true;
        last_wifi_check = millis();
      } else {
        // Print progress without blocking the audio static loops
        if (millis() - last_wifi_check > 500) {
          last_wifi_check = millis();
          Serial.print(".");
        }
      }
    } else {
      if (reconnecting) {
        Serial.println("\nWiFi Reconnected!");
        reconnecting = false;
      }

      // Reconnect to stream every 5s if we are connected to WiFi but still in static tuning mode
      if (millis() - reconnect_timer > 5000) {
        reconnect_timer = millis();
        Serial.println("Attempting to reconnect stream...");
        
        turnOnRadio();

        // If turnOnRadio succeeded, it sets is_playing=true and is_tuning=false.
        // If it failed, it leaves is_tuning=true so we keep playing static and retry.
      }
    }
  }

  // --- Animation and Display Updates ---
  bool needs_draw = false;
  if (is_tuning) {
    needs_draw = true;
  }
  // If is_playing, we just stay wherever we landed.

  if (needs_draw) {
    unsigned long disp_ms = is_tuning ? 50UL : 100UL;
    if (millis() - display_update_timer > disp_ms) {
      display_update_timer = millis();

      // Update animation phase here (tied to frame rate)
      if (is_tuning) {
        view_phase += 0.05f;
        // Wrap at 2*PI to prevent precision loss over time
        if (view_phase > 6.28318f)
          view_phase -= 6.28318f;
        view_angle = 90.0f + 60.0f * sinf(view_phase);
      }

      drawDisplay();
    }
  }
}
