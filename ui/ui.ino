#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ezButton.h>

// ================== PINS ==================
#define CLK_PIN     D7
#define DT_PIN      D6
#define SW_PIN      D5
#define HEATER_PIN  D0

ezButton button(SW_PIN);

// ================== UI STATE ==================
enum UiState {
  UI_HOME,
  UI_MENU,
  UI_STATUS,
  UI_SETTINGS,
  UI_SET_TEMP,
  UI_HEATER_MODE,
  UI_MANUAL_HEATER,
  UI_HYSTERESIS,
  UI_WIFI
};

UiState uiState = UI_HOME;

// ================== HEATER MODE ==================
enum HeaterMode {
  HEATER_AUTO,
  HEATER_MANUAL
};

HeaterMode heaterMode = HEATER_AUTO;
bool manualHeaterOn = false;   // Used only in MANUAL mode

// ================== MENUS ==================
const char* menuItems[] = { "Status", "Settings", "WiFi", "Back" };
const int MENU_SIZE = 4;
int menuIndex = 0;

const char* settingsItems[] = { "Set Temperature", "Heater Mode", "Hysteresis", "Back" };
const int SETTINGS_SIZE = 4;
int settingsIndex = 0;

const char* heaterModeItems[] = { "AUTO", "MANUAL" };
int heaterModeIndex = 0;

const char* wifiItems[] = { "Reconfigure WiFi", "Back" };
int wifiIndex = 0;

// ================== DATA ==================
float liveTemp   = 37.5;   // replace later with DS18B20
float liveHum    = 55.0;
float setTemp    = 37.5;
float hysteresis = 0.3;

bool heaterOn = false;

// ================== TIMERS ==================
unsigned long lastUiActivity   = 0;
unsigned long lastHeaterUpdate = 0;

const unsigned long UI_TIMEOUT      = 10000;
const unsigned long HEATER_INTERVAL = 1000;

int prevCLK;

// ================== WIFI ==================
WiFiManager wm;

// ================== HELPERS ==================
void clearScreen() {
  Serial.println("\n\n\n\n\n");
}

const char* wifiQuality(int rssi) {
  if (rssi >= -55) return "EXCELLENT";
  if (rssi >= -65) return "GOOD";
  if (rssi >= -75) return "FAIR";
  return "POOR";
}

// ================== HEATER CONTROL ==================
void updateHeaterControl() {

  if (heaterMode == HEATER_AUTO) {
    if (liveTemp <= setTemp - hysteresis)
      heaterOn = true;
    else if (liveTemp >= setTemp + hysteresis)
      heaterOn = false;
  }
  else { // MANUAL
    heaterOn = manualHeaterOn;
  }

  digitalWrite(HEATER_PIN, heaterOn ? HIGH : LOW);
}

// ================== SCREENS ==================
void drawHome() {
  clearScreen();
  Serial.println("===== EGG INCUBATOR =====");
  Serial.print("Temp   : "); Serial.print(liveTemp, 1); Serial.println(" C");
  Serial.print("Hum    : "); Serial.print(liveHum, 0); Serial.println(" %");
  Serial.print("Heater : "); Serial.println(heaterOn ? "ON" : "OFF");
  Serial.print("WiFi   : ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
  Serial.println("-------------------------");
  Serial.println("Press encoder -> MENU");
}

void drawMenu() {
  clearScreen();
  Serial.println("==== MAIN MENU ====");
  for (int i = 0; i < MENU_SIZE; i++) {
    Serial.print(i == menuIndex ? "> " : "  ");
    Serial.println(menuItems[i]);
  }
}

void drawStatus() {
  clearScreen();
  Serial.println("------ STATUS ------");
  Serial.print("Temp   : "); Serial.print(liveTemp, 1); Serial.println(" C");
  Serial.print("Hum    : "); Serial.print(liveHum, 0); Serial.println(" %");
  Serial.print("Heater : "); Serial.println(heaterOn ? "ON" : "OFF");

  Serial.print("WiFi   : ");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("CONNECTED");
    Serial.print("Signal : ");
    Serial.println(wifiQuality(WiFi.RSSI()));
  } else {
    Serial.println("DISCONNECTED");
  }

  Serial.println("--------------------");
  Serial.println("Press -> BACK");
}

void drawSettings() {
  clearScreen();
  Serial.println("----- SETTINGS -----");
  for (int i = 0; i < SETTINGS_SIZE; i++) {
    Serial.print(i == settingsIndex ? "> " : "  ");
    Serial.println(settingsItems[i]);
  }
}

void drawSetTemp() {
  clearScreen();
  Serial.println("-- SET TEMPERATURE --");
  Serial.print("Target : "); Serial.print(setTemp, 1); Serial.println(" C");
  Serial.println("Rotate : Change");
  Serial.println("Press  : Save & Back");
}

void drawHeaterMode() {
  clearScreen();
  Serial.println("-- HEATER MODE --");
  for (int i = 0; i < 2; i++) {
    Serial.print(i == heaterModeIndex ? "> " : "  ");
    Serial.println(heaterModeItems[i]);
  }
}

void drawManualHeater() {
  clearScreen();
  Serial.println("-- MANUAL HEATER --");
  Serial.print("State : ");
  Serial.println(manualHeaterOn ? "ON" : "OFF");
  Serial.println("------------------");
  Serial.println("Rotate : Change");
  Serial.println("Press  : Save & Exit");
}

void drawHysteresis() {
  clearScreen();
  Serial.println("-- HYSTERESIS --");
  Serial.print("Value : ±"); Serial.print(hysteresis, 1); Serial.println(" C");
  Serial.println("Rotate : Change");
  Serial.println("Press  : Save & Back");
}

void drawWiFi() {
  clearScreen();
  Serial.println("-- WiFi --");
  for (int i = 0; i < 2; i++) {
    Serial.print(i == wifiIndex ? "> " : "  ");
    Serial.println(wifiItems[i]);
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);

  pinMode(CLK_PIN, INPUT_PULLUP);
  pinMode(DT_PIN, INPUT_PULLUP);
  pinMode(HEATER_PIN, OUTPUT);

  button.setDebounceTime(50);

  prevCLK = digitalRead(CLK_PIN);
  lastUiActivity = millis();

  wm.autoConnect("EggIncubator_Setup");
  drawHome();
}

// ================== LOOP ==================
void loop() {

  button.loop();

  if (millis() - lastHeaterUpdate > HEATER_INTERVAL) {
    lastHeaterUpdate = millis();
    updateHeaterControl();
  }

  int clk = digitalRead(CLK_PIN);

  // -------- ROTATION --------
  if (clk != prevCLK && clk == HIGH) {
    bool cw = digitalRead(DT_PIN) == LOW;
    lastUiActivity = millis();

    if (uiState == UI_MENU) {
      menuIndex += cw ? 1 : -1;
      if (menuIndex < 0) menuIndex = MENU_SIZE - 1;
      if (menuIndex >= MENU_SIZE) menuIndex = 0;
      drawMenu();
    }
    else if (uiState == UI_SETTINGS) {
      settingsIndex += cw ? 1 : -1;
      if (settingsIndex < 0) settingsIndex = SETTINGS_SIZE - 1;
      if (settingsIndex >= SETTINGS_SIZE) settingsIndex = 0;
      drawSettings();
    }
    else if (uiState == UI_SET_TEMP) {
      setTemp += cw ? 0.1 : -0.1;
      if (setTemp < 30.0) setTemp = 30.0;
      if (setTemp > 40.0) setTemp = 40.0;
      drawSetTemp();
    }
    else if (uiState == UI_HEATER_MODE) {
      heaterModeIndex += cw ? 1 : -1;
      if (heaterModeIndex < 0) heaterModeIndex = 1;
      if (heaterModeIndex > 1) heaterModeIndex = 0;
      drawHeaterMode();
    }
    else if (uiState == UI_MANUAL_HEATER) {
      manualHeaterOn = !manualHeaterOn;
      drawManualHeater();
    }
    else if (uiState == UI_HYSTERESIS) {
      hysteresis += cw ? 0.1 : -0.1;
      if (hysteresis < 0.1) hysteresis = 0.1;
      if (hysteresis > 1.0) hysteresis = 1.0;
      drawHysteresis();
    }
    else if (uiState == UI_WIFI) {
      wifiIndex += cw ? 1 : -1;
      if (wifiIndex < 0) wifiIndex = 1;
      if (wifiIndex > 1) wifiIndex = 0;
      drawWiFi();
    }
  }

  prevCLK = clk;

  // -------- BUTTON --------
  if (button.isPressed()) {
    lastUiActivity = millis();

    if (uiState == UI_HOME) {
      uiState = UI_MENU;
      drawMenu();
    }
    else if (uiState == UI_MENU) {
      if (menuIndex == 0) { uiState = UI_STATUS; drawStatus(); }
      else if (menuIndex == 1) { uiState = UI_SETTINGS; drawSettings(); }
      else if (menuIndex == 2) { wifiIndex = 0; uiState = UI_WIFI; drawWiFi(); }
      else { uiState = UI_HOME; drawHome(); }
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
      if (heaterModeIndex == 0) {
        heaterMode = HEATER_AUTO;
        uiState = UI_SETTINGS;
        drawSettings();
      } else {
        heaterMode = HEATER_MANUAL;
        uiState = UI_MANUAL_HEATER;
        drawManualHeater();
      }
    }
    else if (uiState == UI_MANUAL_HEATER) {
      uiState = UI_SETTINGS;
      drawSettings();
    }
    else if (uiState == UI_SET_TEMP || uiState == UI_HYSTERESIS) {
      uiState = UI_SETTINGS;
      drawSettings();
    }
    else if (uiState == UI_WIFI) {
      if (wifiIndex == 0) {
        wm.resetSettings();
        ESP.restart();
      } else {
        uiState = UI_MENU;
        drawMenu();
      }
    }
    else {
      uiState = UI_MENU;
      drawMenu();
    }
  }

  // -------- AUTO HOME --------
  if (uiState != UI_HOME && millis() - lastUiActivity > UI_TIMEOUT) {
    uiState = UI_HOME;
    drawHome();
  }
}
