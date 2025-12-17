#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ezButton.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ================= OLED =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3D
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ================== PINS ==================
#define CLK_PIN     D7
#define DT_PIN      D6
#define SW_PIN      D5
#define HEATER_PIN  D0

// ================= ENCODER =================
int CLK_state;
int prev_CLK_state;
ezButton button(SW_PIN);

// ================== UI STATE ==================
enum UiState {
  UI_HOME,
  UI_MENU,
  UI_STATUS,
  UI_WIFI_MENU,
  UI_SETTINGS,
  UI_SET_TEMP,
  UI_HEATER_MODE,
  UI_MANUAL_HEATER,
  UI_HYSTERESIS
};

UiState uiState = UI_HOME;
UiState previousState = UI_HOME;   // <<< FIX

// ================== MENUS ==================
const char* menuItems[] = { "Status", "WiFi", "Settings", "Back" };
const char* wifiMenuItems[] = { "Connect / Reconnect", "Status", "Back" };
const char* settingsItems[] = { "Set Temperature", "Heater Mode", "Hysteresis", "Back" };
const char* heaterModeItems[] = { "AUTO", "MANUAL" };

int menuIndex = 0;
int wifiMenuIndex = 0;
int settingsIndex = 0;
int heaterModeIndex = 0;

// ================== DATA ==================
float liveTemp   = 37.5;
float liveHum    = 55.0;
float setTemp    = 37.5;
float hysteresis = 0.3;

bool heaterOn = false;
bool manualHeaterOn = false;

enum HeaterMode { HEATER_AUTO, HEATER_MANUAL };
HeaterMode heaterMode = HEATER_AUTO;

// ================== TIMERS ==================
unsigned long lastUiActivity   = 0;
unsigned long lastHeaterUpdate = 0;

const unsigned long UI_TIMEOUT      = 10000;
const unsigned long HEATER_INTERVAL = 1000;

// ================== WIFI ==================
WiFiManager wm;

// ================= HELPERS =================
const char* wifiQuality(int rssi) {
  if (rssi >= -55) return "EXCELLENT";
  if (rssi >= -65) return "GOOD";
  if (rssi >= -75) return "FAIR";
  return "POOR";
}

// ================= HEATER CONTROL =================
void updateHeaterControl() {
  if (heaterMode == HEATER_AUTO) {
    if (liveTemp <= setTemp - hysteresis) heaterOn = true;
    else if (liveTemp >= setTemp + hysteresis) heaterOn = false;
  } else {
    heaterOn = manualHeaterOn;
  }
  digitalWrite(HEATER_PIN, heaterOn ? HIGH : LOW);
}

// ================= OLED HELPERS =================
void drawHeader(const char* title) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(title);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
}

// ================= OLED SCREENS =================
void drawHome() {
  drawHeader("EGG INCUBATOR");

  display.setCursor(0, 14);
  display.print("Temp  : "); display.print(liveTemp, 1); display.print(" C");

  display.setCursor(0, 24);
  display.print("Hum   : "); display.print(liveHum, 0); display.print(" %");

  display.setCursor(0, 34);
  display.print("Mode  : "); display.print(heaterMode == HEATER_AUTO ? "AUTO" : "MANUAL");

  display.setCursor(0, 44);
  display.print("Heater: "); display.print(heaterOn ? "ON" : "OFF");

  display.setCursor(0, 54);
  display.print("WiFi  : ");
  display.print(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");

  display.display();
}

void drawMenu() {
  drawHeader("MAIN MENU");
  for (int i = 0; i < 4; i++) {
    display.setCursor(0, 14 + i * 10);
    display.print(i == menuIndex ? "> " : "  ");
    display.println(menuItems[i]);
  }
  display.display();
}

void drawWiFiMenu() {
  drawHeader("WIFI");
  for (int i = 0; i < 3; i++) {
    display.setCursor(0, 14 + i * 10);
    display.print(i == wifiMenuIndex ? "> " : "  ");
    display.println(wifiMenuItems[i]);
  }
  display.display();
}

void drawStatus() {
  drawHeader("STATUS");

  display.setCursor(0, 14);
  display.print("Temp  : "); display.print(liveTemp, 1); display.print(" C");

  display.setCursor(0, 24);
  display.print("Hum   : "); display.print(liveHum, 0); display.print(" %");

  display.setCursor(0, 34);
  display.print("Heater: "); display.print(heaterOn ? "ON" : "OFF");

  display.setCursor(0, 44);
  display.print("Signal: ");
  display.print(WiFi.status() == WL_CONNECTED ? wifiQuality(WiFi.RSSI()) : "---");

  display.display();
}

void drawSettings() {
  drawHeader("SETTINGS");
  for (int i = 0; i < 4; i++) {
    display.setCursor(0, 14 + i * 10);
    display.print(i == settingsIndex ? "> " : "  ");
    display.println(settingsItems[i]);
  }
  display.display();
}

void drawSetTemp() {
  drawHeader("SET TEMP");
  display.setCursor(0, 28);
  display.print("Target: "); display.print(setTemp, 1); display.print(" C");
  display.display();
}

void drawHeaterMode() {
  drawHeader("HEATER MODE");
  for (int i = 0; i < 2; i++) {
    display.setCursor(0, 28 + i * 10);
    display.print(i == heaterModeIndex ? "> " : "  ");
    display.println(heaterModeItems[i]);
  }
  display.display();
}

void drawManualHeater() {
  drawHeader("MANUAL HEATER");
  display.setCursor(0, 32);
  display.print("State: ");
  display.print(manualHeaterOn ? "ON" : "OFF");
  display.display();
}

void drawHysteresis() {
  drawHeader("HYSTERESIS");
  display.setCursor(0, 32);
  display.print("Value: +-");
  display.print(hysteresis, 1);
  display.print(" C");
  display.display();
}

// ================= ENCODER =================
int readEncoder() {
  int move = 0;
  CLK_state = digitalRead(CLK_PIN);
  if (CLK_state != prev_CLK_state && CLK_state == HIGH) {
    move = digitalRead(DT_PIN) ? -1 : +1;
  }
  prev_CLK_state = CLK_state;
  return move;
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);

  pinMode(CLK_PIN, INPUT);
  pinMode(DT_PIN, INPUT);
  pinMode(HEATER_PIN, OUTPUT);
  button.setDebounceTime(50);

  Wire.begin(D2, D1);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  prev_CLK_state = digitalRead(CLK_PIN);
  lastUiActivity = millis();
  lastHeaterUpdate = millis();

  wm.autoConnect("EggIncubator_Setup");

  updateHeaterControl();
  drawHome();
}

// ================== LOOP ==================
void loop() {
  button.loop();

  if (millis() - lastHeaterUpdate > HEATER_INTERVAL) {
    lastHeaterUpdate = millis();
    updateHeaterControl();
    if (uiState == UI_HOME) drawHome();
  }

  int enc = readEncoder();
  if (enc != 0) lastUiActivity = millis();

  if (enc != 0) {
    if (uiState == UI_MENU) {
      menuIndex = (menuIndex + enc + 4) % 4;
      drawMenu();
    }
    else if (uiState == UI_WIFI_MENU) {
      wifiMenuIndex = (wifiMenuIndex + enc + 3) % 3;
      drawWiFiMenu();
    }
    else if (uiState == UI_SETTINGS) {
      settingsIndex = (settingsIndex + enc + 4) % 4;
      drawSettings();
    }
    else if (uiState == UI_SET_TEMP) {
      setTemp = constrain(setTemp + enc * 0.1, 30.0, 40.0);
      drawSetTemp();
    }
    else if (uiState == UI_HEATER_MODE) {
      heaterModeIndex = (heaterModeIndex + enc + 2) % 2;
      drawHeaterMode();
    }
    else if (uiState == UI_MANUAL_HEATER) {
      manualHeaterOn = !manualHeaterOn;
      drawManualHeater();
    }
    else if (uiState == UI_HYSTERESIS) {
      hysteresis = constrain(hysteresis + enc * 0.1, 0.1, 1.0);
      drawHysteresis();
    }
  }

  if (button.isPressed()) {
    lastUiActivity = millis();

    if (uiState == UI_HOME) {
      uiState = UI_MENU;
      drawMenu();
    }
    else if (uiState == UI_MENU) {
      if (menuIndex == 0) { previousState = UI_MENU; uiState = UI_STATUS; drawStatus(); }
      else if (menuIndex == 1) { wifiMenuIndex = 0; uiState = UI_WIFI_MENU; drawWiFiMenu(); }
      else if (menuIndex == 2) { uiState = UI_SETTINGS; drawSettings(); }
      else { uiState = UI_HOME; drawHome(); }
    }
    else if (uiState == UI_WIFI_MENU) {
      if (wifiMenuIndex == 0) { wm.resetSettings(); ESP.restart(); }
      else if (wifiMenuIndex == 1) { previousState = UI_WIFI_MENU; uiState = UI_STATUS; drawStatus(); }
      else { uiState = UI_MENU; drawMenu(); }
    }
    else if (uiState == UI_STATUS) {
      uiState = previousState;
      if (uiState == UI_MENU) drawMenu();
      else if (uiState == UI_WIFI_MENU) drawWiFiMenu();
    }
    else if (uiState == UI_SETTINGS) {
      if (settingsIndex == 0) { uiState = UI_SET_TEMP; drawSetTemp(); }
      else if (settingsIndex == 1) {
        heaterModeIndex = heaterMode;
        uiState = UI_HEATER_MODE;
        drawHeaterMode();
      }
      else if (settingsIndex == 2) { uiState = UI_HYSTERESIS; drawHysteresis(); }
      else { uiState = UI_MENU; drawMenu(); }
    }
    else if (uiState == UI_HEATER_MODE) {
      heaterMode = heaterModeIndex == 0 ? HEATER_AUTO : HEATER_MANUAL;
      uiState = heaterMode == HEATER_MANUAL ? UI_MANUAL_HEATER : UI_SETTINGS;
      heaterMode == HEATER_MANUAL ? drawManualHeater() : drawSettings();
    }
    else {
      uiState = UI_SETTINGS;
      drawSettings();
    }
  }

  if (uiState != UI_HOME && millis() - lastUiActivity > UI_TIMEOUT) {
    uiState = UI_HOME;
    drawHome();
  }
}
