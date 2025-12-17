#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ezButton.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#include <EEPROM.h>
#include <time.h>

// ================= EEPROM =================
#define EEPROM_SIZE 64

#define ADDR_SET_TEMP 0      // float (4 bytes)
#define ADDR_HYSTERESIS 4    // float (4 bytes)
#define ADDR_HEATER_MODE 8   // uint8_t
#define ADDR_MANUAL_STATE 9  // uint8_t

uint8_t eepromHeaterMode = 0;
uint8_t eepromManualState = 0;

// ================= NTP =================

const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 19800;   // IST
const int   DAYLIGHT_OFFSET_SEC = 0;

bool timeValid = false;
struct tm timeinfo;   // ✅ GLOBAL


// ================= OLED =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3D
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ================== PINS ==================
#define CLK_PIN D7
#define DT_PIN D6
#define SW_PIN D5
#define HEATER_PIN D0
#define ONE_WIRE_BUS D4

// ================= ENCODER =================
int CLK_state;
int prev_CLK_state;
ezButton button(SW_PIN);

// ================== DS18B20 ==================
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

float rawTemp = 0.0;
float filteredTemp = 0.0;
float liveTemp = 0.0;
bool sensorValid = false;

unsigned long lastTempRequest = 0;
unsigned long tempRequestTime = 0;
bool tempRequested = false;
const unsigned long TEMP_INTERVAL = 1000;

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

// ================== MENUS ==================
const char* menuItems[] = { "Status", "WiFi", "Settings", "Back" };
const char* wifiMenuItems[] = { "Connect / Reconnect", "Status", "Back" };
const char* settingsItems[] = { "Set Temperature", "Heater Mode", "Hysteresis", "Back" };
const char* heaterModeItems[] = { "AUTO", "MANUAL" };

int menuIndex = 0;
int wifiMenuIndex = 0;
int settingsIndex = 0;
int heaterModeIndex = 0;

// ===== Manual Heater =====
int manualSelectIndex = 0;  // 0=OFF 1=ON

// ================== DATA ==================
float liveHum = 55.0;
float setTemp = 37.5;
float hysteresis = 0.3;

bool heaterOn = false;
bool manualHeaterOn = false;

enum HeaterMode { HEATER_AUTO,
                  HEATER_MANUAL };
HeaterMode heaterMode = HEATER_AUTO;

// ================== TIMERS ==================
unsigned long lastUiActivity = 0;
unsigned long lastHeaterUpdate = 0;

const unsigned long UI_TIMEOUT = 10000;
const unsigned long HEATER_INTERVAL = 1000;

// ================== WIFI ==================
WiFiManager wm;

// ================= TEMPERATURE TASK =================
void temperatureTask() {
  unsigned long now = millis();

  if (!tempRequested && now - lastTempRequest >= TEMP_INTERVAL) {
    sensors.requestTemperatures();
    tempRequestTime = now;
    tempRequested = true;
    lastTempRequest = now;
  }

  if (tempRequested && now - tempRequestTime >= 750) {
    float t = sensors.getTempCByIndex(0);
    if (t > -40 && t < 100 && t != DEVICE_DISCONNECTED_C) {
      sensorValid = true;
      rawTemp = t;
      filteredTemp = filteredTemp * 0.7 + rawTemp * 0.3;
      liveTemp = filteredTemp;
    } else {
      sensorValid = false;
    }
    tempRequested = false;
  }
}

// ================= HEATER CONTROL =================
void updateHeaterControl() {
  if (!sensorValid) {
    heaterOn = false;
    digitalWrite(HEATER_PIN, LOW);
    return;
  }

  if (heaterMode == HEATER_AUTO) {
    if (liveTemp <= setTemp - hysteresis) heaterOn = true;
    else if (liveTemp >= setTemp + hysteresis) heaterOn = false;
  } else {
    heaterOn = manualHeaterOn;
  }

  digitalWrite(HEATER_PIN, heaterOn ? HIGH : LOW);
}

void loadSettingsFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);

  EEPROM.get(ADDR_SET_TEMP, setTemp);
  EEPROM.get(ADDR_HYSTERESIS, hysteresis);

  EEPROM.get(ADDR_HEATER_MODE, eepromHeaterMode);
  EEPROM.get(ADDR_MANUAL_STATE, eepromManualState);

  // ---- Safety defaults ----
  if (setTemp < 30.0 || setTemp > 40.0) setTemp = 37.5;
  if (hysteresis < 0.1 || hysteresis > 1.0) hysteresis = 0.3;

  if (eepromHeaterMode > 1) eepromHeaterMode = 0;
  heaterMode = (HeaterMode)eepromHeaterMode;

  manualHeaterOn = (eepromManualState == 1);
}


void saveSettingsToEEPROM() {
  eepromHeaterMode = (uint8_t)heaterMode;
  eepromManualState = manualHeaterOn ? 1 : 0;

  EEPROM.put(ADDR_SET_TEMP, setTemp);
  EEPROM.put(ADDR_HYSTERESIS, hysteresis);
  EEPROM.put(ADDR_HEATER_MODE, eepromHeaterMode);
  EEPROM.put(ADDR_MANUAL_STATE, eepromManualState);

  EEPROM.commit();
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
  display.print("Temp  : ");
  if (sensorValid) display.print(liveTemp, 1), display.print(" C");
  else display.print("SENSOR ERR");

  display.setCursor(0, 24);
  display.print("Hum   : ");
  display.print(liveHum, 0);
  display.print(" %");

  display.setCursor(0, 34);
  display.print("Mode  : ");
  display.print(heaterMode == HEATER_AUTO ? "AUTO" : "MANUAL");

  display.setCursor(0, 44);
  display.print("Heater: ");
  display.print(heaterOn ? "ON" : "OFF");

  display.setCursor(0, 54);
  display.print("WiFi  : ");
  display.print(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
 if (timeValid) {
  char timeStr[10];
  sprintf(timeStr, "%02d:%02d",
          timeinfo.tm_hour,
          timeinfo.tm_min );

  display.setCursor(90, 0);
  display.print(timeStr);
}


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

void drawWifiMenu() {
  drawHeader("WIFI");
  for (int i = 0; i < 3; i++) {
    display.setCursor(0, 14 + i * 10);
    display.print(i == wifiMenuIndex ? "> " : "  ");
    display.println(wifiMenuItems[i]);
  }
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
  display.setCursor(0, 32);
  display.print("Target: ");
  display.print(setTemp, 1);
  display.print(" C");
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
  display.setCursor(0, 28);
  display.print(manualSelectIndex == 1 ? "> ON" : "  ON");
  display.setCursor(0, 40);
  display.print(manualSelectIndex == 0 ? "> OFF" : "  OFF");
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
  if (CLK_state != prev_CLK_state && CLK_state == HIGH)
    move = digitalRead(DT_PIN) ? -1 : +1;
  prev_CLK_state = CLK_state;
  return move;
}
bool updateTime(struct tm& timeinfo) {
  if (!getLocalTime(&timeinfo)) {
    timeValid = false;
    return false;
  }
  timeValid = true;
  return true;
}


// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(CLK_PIN, INPUT);
  pinMode(DT_PIN, INPUT);
  pinMode(SW_PIN, INPUT);
  pinMode(HEATER_PIN, OUTPUT);

  button.setDebounceTime(50);

  Wire.begin(D2, D1);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(false);

  loadSettingsFromEEPROM();

  prev_CLK_state = digitalRead(CLK_PIN);
  lastUiActivity = millis();
  lastHeaterUpdate = millis();

  wm.autoConnect("EggIncubator_Setup");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);


  drawHome();
}

// ================= LOOP =================
void loop() {
  button.loop();
  temperatureTask();

  static unsigned long lastTimeCheck = 0;

if (WiFi.status() == WL_CONNECTED && millis() - lastTimeCheck > 1000) {
  lastTimeCheck = millis();
  if (getLocalTime(&timeinfo)) {
    timeValid = true;
  } else {
    timeValid = false;
  }
}


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
    } else if (uiState == UI_WIFI_MENU) {
      wifiMenuIndex = (wifiMenuIndex + enc + 3) % 3;
      drawWifiMenu();
    } else if (uiState == UI_SETTINGS) {
      settingsIndex = (settingsIndex + enc + 4) % 4;
      drawSettings();
    } else if (uiState == UI_SET_TEMP) {
      setTemp = constrain(setTemp + enc * 0.1, 30.0, 40.0);
      drawSetTemp();
    } else if (uiState == UI_HEATER_MODE) {
      heaterModeIndex = (heaterModeIndex + enc + 2) % 2;
      drawHeaterMode();
    } else if (uiState == UI_MANUAL_HEATER) {
      manualSelectIndex = (manualSelectIndex + enc + 2) % 2;
      drawManualHeater();
    } else if (uiState == UI_HYSTERESIS) {
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
      if (menuIndex == 0) {
        uiState = UI_HOME;
        drawHome();
      } else if (menuIndex == 1) {
        wifiMenuIndex = 0;
        uiState = UI_WIFI_MENU;
        drawWifiMenu();
      } else if (menuIndex == 2) {
        settingsIndex = 0;
        uiState = UI_SETTINGS;
        drawSettings();
      } else {
        uiState = UI_HOME;
        drawHome();
      }
    }

    else if (uiState == UI_WIFI_MENU) {
      if (wifiMenuIndex == 0) {
        wm.startConfigPortal("EggIncubator_Setup");
        drawWifiMenu();
      } else if (wifiMenuIndex == 1) {  // Status
        uiState = UI_HOME;
        drawHome();
      } else {  // Back
        uiState = UI_MENU;
        drawMenu();
      }
    }


    else if (uiState == UI_SETTINGS) {
      if (settingsIndex == 0) {
        uiState = UI_SET_TEMP;
        drawSetTemp();
      } else if (settingsIndex == 1) {
        heaterModeIndex = heaterMode;
        uiState = UI_HEATER_MODE;
        drawHeaterMode();
      } else if (settingsIndex == 2) {
        uiState = UI_HYSTERESIS;
        drawHysteresis();
      } else {
        uiState = UI_MENU;
        drawMenu();
      }
    }

    else if (uiState == UI_HEATER_MODE) {
      heaterMode = heaterModeIndex == 0 ? HEATER_AUTO : HEATER_MANUAL;
      saveSettingsToEEPROM();  // ✅ SAVE MODE

      if (heaterMode == HEATER_MANUAL) {
        manualSelectIndex = manualHeaterOn ? 1 : 0;
        uiState = UI_MANUAL_HEATER;
        drawManualHeater();
      } else {
        uiState = UI_SETTINGS;
        drawSettings();
      }
    } else if (uiState == UI_MANUAL_HEATER) {
      manualHeaterOn = (manualSelectIndex == 1);
      saveSettingsToEEPROM();  // ✅ SAVE MANUAL STATE
      uiState = UI_SETTINGS;
      drawSettings();
    } else if (uiState == UI_SET_TEMP) {
      saveSettingsToEEPROM();  // SAVE already done by encoder
      uiState = UI_SETTINGS;
      drawSettings();
    } else if (uiState == UI_HYSTERESIS) {
      saveSettingsToEEPROM();
      uiState = UI_SETTINGS;
      drawSettings();
    }
  }

  if (uiState != UI_HOME && millis() - lastUiActivity > UI_TIMEOUT) {
    uiState = UI_HOME;
    drawHome();
  }
}
