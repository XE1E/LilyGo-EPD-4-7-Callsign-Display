/*
 * LilyGo EPD 4.7" Callsign Display
 *
 * Amateur radio callsign display for LilyGo T5 4.7" e-paper (non-touch version)
 * Features:
 * - Large callsign letters (configurable 4-6 characters)
 * - World timezone map as secondary screen
 * - BOOT button to toggle between screens
 * - WiFi AP mode for web configuration (no recompilation needed)
 * - Deep sleep for battery operation
 *
 * Hardware: LilyGo T5 4.7" S3 (ESP32-S3, 960x540 e-paper)
 *
 * Configuration:
 * 1. Power on device
 * 2. Connect to WiFi "Callsign-Setup" (password: callsign123)
 * 3. Open http://192.168.4.1 in browser
 * 4. Enter your callsign and save
 *
 * Navigation:
 * - Press BOOT button: Toggle Callsign <-> World Map
 * - Hold BOOT 3 seconds: Enter configuration mode (AP)
 *
 * Board settings (Arduino IDE):
 * - Board: ESP32S3 Dev Module
 * - USB CDC On Boot: Enable
 * - Flash Size: 16MB (128Mb)
 * - Partition Scheme: 16M Flash (3M APP/9.9MB FATFS)
 * - PSRAM: OPI PSRAM
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "epd_driver.h"
#include "esp_sleep.h"

// Fonts
#include "opensans12b.h"
#include "opensans18b.h"
#include "opensans24b.h"

// ============== FORWARD DECLARATIONS ==============
// Required before including screen modules

// Grayscale colors
#define White   0xFF
#define LightGrey 0xBB
#define Grey    0x88
#define DarkGrey 0x44
#define Black   0x00

// Alignment enum (used by screen modules)
enum alignment { LEFT, RIGHT, CENTER };

// Display state
uint8_t *framebuffer = NULL;
GFXfont currentFont;

// Forward declarations for drawing functions used by screen modules
void setFont(GFXfont const &font);
void drawString(int x, int y, String text, alignment align);
void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void fillCircle(int x, int y, int r, uint8_t color);
void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color);
void drawFastHLine(int16_t x, int16_t y, int length, uint16_t color);

// Screen modules (after forward declarations)
#include "callsign.h"
#include "worldclock.h"

// ============== HARDWARE PINS ==============
#define BOOT_BUTTON_PIN 0   // GPIO0 is the BOOT button on ESP32-S3

// ============== CONFIGURATION ==============
#define AP_SSID       "Callsign-Setup"
#define AP_PASSWORD   "callsign123"
#define CONFIG_HOLD_TIME_MS 3000  // Hold BOOT for 3 seconds to enter config

// ============== DISPLAY ==============
#define SCREEN_WIDTH  960
#define SCREEN_HEIGHT 540

// ============== STATE ==============
enum ScreenState {
  SCREEN_CALLSIGN = 0,
  SCREEN_WORLDMAP = 1
};

// RTC memory persists through deep sleep
RTC_DATA_ATTR ScreenState currentScreen = SCREEN_CALLSIGN;
RTC_DATA_ATTR bool configuredOnce = false;

// Runtime state
Preferences preferences;
WebServer server(80);
bool apModeActive = false;

// Callsign configuration (loaded from preferences)
String callsignText = "CALL";
String callsignLine1 = "Your Name";
String callsignLine2 = "Your Location";
int sleepMinutes = 60;  // Deep sleep interval

// ============== DISPLAY FUNCTIONS ==============

void setFont(GFXfont const &font) {
  currentFont = font;
}

void drawString(int x, int y, String text, alignment align) {
  char *data = const_cast<char*>(text.c_str());
  int x1, y1;
  int w, h;
  int xx = x, yy = y;

  get_text_bounds(&currentFont, data, &xx, &yy, &x1, &y1, &w, &h, NULL);

  if (align == RIGHT)  x = x - w;
  if (align == CENTER) x = x - w / 2;

  int cursor_y = y + h;
  write_string(&currentFont, data, &x, &cursor_y, framebuffer);
}

void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  epd_fill_rect(x, y, w, h, color, framebuffer);
}

void fillCircle(int x, int y, int r, uint8_t color) {
  epd_fill_circle(x, y, r, color, framebuffer);
}

void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color) {
  epd_fill_triangle(x0, y0, x1, y1, x2, y2, color, framebuffer);
}

void drawFastHLine(int16_t x, int16_t y, int length, uint16_t color) {
  epd_draw_hline(x, y, length, color, framebuffer);
}

// ============== CONFIGURATION STORAGE ==============

void loadConfig() {
  preferences.begin("callsign", true);  // Read-only
  callsignText = preferences.getString("text", "CALL");
  callsignLine1 = preferences.getString("line1", "Your Name");
  callsignLine2 = preferences.getString("line2", "Your Location");
  sleepMinutes = preferences.getInt("sleep", 60);
  configuredOnce = preferences.getBool("configured", false);
  preferences.end();

  Serial.printf("Config loaded: %s / %s / %s / sleep=%d min\n",
    callsignText.c_str(), callsignLine1.c_str(), callsignLine2.c_str(), sleepMinutes);
}

void saveConfig() {
  preferences.begin("callsign", false);  // Read-write
  preferences.putString("text", callsignText);
  preferences.putString("line1", callsignLine1);
  preferences.putString("line2", callsignLine2);
  preferences.putInt("sleep", sleepMinutes);
  preferences.putBool("configured", true);
  preferences.end();

  Serial.println("Config saved");
}

// ============== WEB SERVER ==============

const char* CONFIG_PAGE = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Callsign Configuration</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 500px; margin: 40px auto; padding: 20px; background: #1a1a2e; color: #eee; }
    h1 { color: #00d4ff; text-align: center; }
    .form-group { margin: 20px 0; }
    label { display: block; margin-bottom: 8px; font-weight: bold; }
    input[type="text"], input[type="number"] {
      width: 100%; padding: 12px; border: 2px solid #333; border-radius: 8px;
      background: #16213e; color: #fff; font-size: 18px; box-sizing: border-box;
    }
    input:focus { border-color: #00d4ff; outline: none; }
    .callsign-input { font-family: monospace; font-size: 28px; text-transform: uppercase; text-align: center; letter-spacing: 8px; }
    button {
      width: 100%; padding: 15px; background: #00d4ff; color: #000; border: none;
      border-radius: 8px; font-size: 18px; font-weight: bold; cursor: pointer; margin-top: 20px;
    }
    button:hover { background: #00b8e6; }
    .info { background: #16213e; padding: 15px; border-radius: 8px; margin-top: 30px; font-size: 14px; }
    .current { color: #00d4ff; }
    .hint { color: #888; font-size: 12px; margin-top: 5px; }
  </style>
</head>
<body>
  <h1>Callsign Display</h1>
  <form action="/save" method="POST">
    <div class="form-group">
      <label>Callsign (4-6 characters)</label>
      <input type="text" name="callsign" class="callsign-input" maxlength="6"
             pattern="[A-Za-z0-9]{4,6}" value="%CALLSIGN%" required>
      <div class="hint">Letters A-Z and numbers 0-9 only</div>
    </div>
    <div class="form-group">
      <label>Line 1 (Name)</label>
      <input type="text" name="line1" maxlength="50" value="%LINE1%">
    </div>
    <div class="form-group">
      <label>Line 2 (Location / Grid)</label>
      <input type="text" name="line2" maxlength="50" value="%LINE2%">
    </div>
    <div class="form-group">
      <label>Display refresh interval (minutes)</label>
      <input type="number" name="sleep" min="1" max="1440" value="%SLEEP%">
      <div class="hint">Device will deep sleep between refreshes to save battery</div>
    </div>
    <button type="submit">Save Configuration</button>
  </form>
  <div class="info">
    <strong>Instructions:</strong><br>
    1. Enter your callsign and info<br>
    2. Click Save - device will restart<br>
    3. Press BOOT button to toggle screens<br>
    4. Hold BOOT 3 sec to return here<br><br>
    <span class="current">Current: %CALLSIGN%</span>
  </div>
</body>
</html>
)rawliteral";

const char* SAVED_PAGE = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Saved</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 500px; margin: 100px auto; padding: 20px;
           background: #1a1a2e; color: #eee; text-align: center; }
    h1 { color: #00ff88; }
    .callsign { font-size: 48px; font-family: monospace; letter-spacing: 8px; margin: 30px 0; color: #00d4ff; }
  </style>
</head>
<body>
  <h1>Configuration Saved!</h1>
  <div class="callsign">%CALLSIGN%</div>
  <p>Device will restart in 3 seconds...</p>
</body>
</html>
)rawliteral";

void handleRoot() {
  String page = CONFIG_PAGE;
  page.replace("%CALLSIGN%", callsignText);
  page.replace("%LINE1%", callsignLine1);
  page.replace("%LINE2%", callsignLine2);
  page.replace("%SLEEP%", String(sleepMinutes));
  server.send(200, "text/html", page);
}

void handleSave() {
  if (server.hasArg("callsign")) {
    callsignText = server.arg("callsign");
    callsignText.toUpperCase();
    // Validate: only A-Z, 0-9
    String validated = "";
    for (int i = 0; i < callsignText.length() && validated.length() < 6; i++) {
      char c = callsignText.charAt(i);
      if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        validated += c;
      }
    }
    callsignText = validated.length() >= 4 ? validated : "CALL";
  }
  if (server.hasArg("line1")) callsignLine1 = server.arg("line1");
  if (server.hasArg("line2")) callsignLine2 = server.arg("line2");
  if (server.hasArg("sleep")) sleepMinutes = server.arg("sleep").toInt();
  if (sleepMinutes < 1) sleepMinutes = 1;
  if (sleepMinutes > 1440) sleepMinutes = 1440;

  saveConfig();

  String page = SAVED_PAGE;
  page.replace("%CALLSIGN%", callsignText);
  server.send(200, "text/html", page);

  delay(3000);
  ESP.restart();
}

void startAPMode() {
  apModeActive = true;

  Serial.println("Starting AP mode for configuration...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  IPAddress IP = WiFi.softAPIP();
  Serial.printf("AP started: %s\n", AP_SSID);
  Serial.printf("Connect to: http://%s\n", IP.toString().c_str());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  // Display AP info on screen
  displayAPInfo();
}

void displayAPInfo() {
  epd_poweron();
  epd_clear();

  fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, White);

  setFont(OpenSans24B);
  drawString(SCREEN_WIDTH / 2, 80, "CONFIGURATION MODE", CENTER);

  setFont(OpenSans18B);
  drawString(SCREEN_WIDTH / 2, 160, "Connect to WiFi:", CENTER);

  setFont(OpenSans24B);
  drawString(SCREEN_WIDTH / 2, 220, AP_SSID, CENTER);

  setFont(OpenSans18B);
  drawString(SCREEN_WIDTH / 2, 280, "Password: " + String(AP_PASSWORD), CENTER);

  drawString(SCREEN_WIDTH / 2, 360, "Then open browser:", CENTER);

  setFont(OpenSans24B);
  drawString(SCREEN_WIDTH / 2, 420, "http://192.168.4.1", CENTER);

  setFont(OpenSans12B);
  drawString(SCREEN_WIDTH / 2, 500, "Press RST button to exit configuration mode", CENTER);

  epd_draw_grayscale_image(epd_full_screen(), framebuffer);
  epd_poweroff();
}

// ============== BUTTON HANDLING ==============

void checkButton() {
  static unsigned long buttonPressStart = 0;
  static bool buttonWasPressed = false;

  bool buttonPressed = (digitalRead(BOOT_BUTTON_PIN) == LOW);

  if (buttonPressed && !buttonWasPressed) {
    // Button just pressed
    buttonPressStart = millis();
    buttonWasPressed = true;
  }
  else if (!buttonPressed && buttonWasPressed) {
    // Button released
    unsigned long pressDuration = millis() - buttonPressStart;
    buttonWasPressed = false;

    if (pressDuration >= CONFIG_HOLD_TIME_MS) {
      // Long press - enter config mode
      Serial.println("Long press detected - entering config mode");
      startAPMode();
    }
    else if (pressDuration >= 50) {
      // Short press - toggle screen
      Serial.println("Short press detected - toggling screen");
      currentScreen = (currentScreen == SCREEN_CALLSIGN) ? SCREEN_WORLDMAP : SCREEN_CALLSIGN;
      displayCurrentScreen();
    }
  }
  else if (buttonPressed && buttonWasPressed) {
    // Button still held - check for long press feedback
    unsigned long pressDuration = millis() - buttonPressStart;
    if (pressDuration >= CONFIG_HOLD_TIME_MS && !apModeActive) {
      // Show feedback that config mode will activate on release
      Serial.println("Hold detected - will enter config mode on release");
    }
  }
}

// ============== DISPLAY SCREENS ==============

void displayCurrentScreen() {
  epd_poweron();
  epd_clear();

  fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, White);

  if (currentScreen == SCREEN_CALLSIGN) {
    // Update callsign.h configuration at runtime
    drawCallsignText(callsignText.c_str(), 50, 30, 120);
    drawCallsignSubtitle(callsignLine1.c_str(), callsignLine2.c_str());
  } else {
    DisplayWorldClockScreen();
  }

  epd_draw_grayscale_image(epd_full_screen(), framebuffer);
  epd_poweroff();
}

// ============== DEEP SLEEP ==============

void enterDeepSleep() {
  Serial.printf("Entering deep sleep for %d minutes...\n", sleepMinutes);

  // Configure wake on BOOT button
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BOOT_BUTTON_PIN, 0);  // Wake on LOW

  // Also wake on timer
  esp_sleep_enable_timer_wakeup(sleepMinutes * 60ULL * 1000000ULL);

  Serial.println("Going to sleep...");
  Serial.flush();

  esp_deep_sleep_start();
}

// ============== SETUP & LOOP ==============

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== Callsign Display Starting ===");

  // Initialize button
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  // Check wake reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.printf("Wake reason: %d\n", wakeup_reason);

  // Initialize display
  epd_init();
  framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
  if (!framebuffer) {
    Serial.println("ERROR: Failed to allocate framebuffer!");
    while (1) delay(1000);
  }
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  // Load configuration
  loadConfig();

  // Check if first boot (never configured) - go directly to AP mode
  if (!configuredOnce) {
    Serial.println("First boot - starting configuration mode");
    startAPMode();
    return;
  }

  // Check if woke by button press
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke by button - checking for long press...");

    // Wait a bit and check if button is still held
    delay(100);
    unsigned long startCheck = millis();
    while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      if (millis() - startCheck >= CONFIG_HOLD_TIME_MS) {
        Serial.println("Long press on wake - entering config mode");
        startAPMode();
        return;
      }
      delay(10);
    }

    // Short press - toggle screen
    Serial.println("Short press on wake - toggling screen");
    currentScreen = (currentScreen == SCREEN_CALLSIGN) ? SCREEN_WORLDMAP : SCREEN_CALLSIGN;
  }

  // Display current screen
  displayCurrentScreen();

  // Enter deep sleep
  enterDeepSleep();
}

void loop() {
  // Only runs in AP mode
  if (apModeActive) {
    server.handleClient();

    // Check for button press to exit AP mode
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      delay(50);  // Debounce
      if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        unsigned long pressStart = millis();
        while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
          delay(10);
        }
        unsigned long pressDuration = millis() - pressStart;

        if (pressDuration < CONFIG_HOLD_TIME_MS && pressDuration >= 50) {
          // Short press in AP mode - restart to normal mode
          Serial.println("Short press in AP mode - restarting...");
          ESP.restart();
        }
      }
    }
  }
}
