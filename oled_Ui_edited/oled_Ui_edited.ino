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

#define ADDR_SET_TEMP 0             // float (4 bytes)
#define ADDR_HYSTERESIS 4           // float (4 bytes)
#define ADDR_HEATER_MODE 8          // uint8_t
#define ADDR_MANUAL_STATE 9         // uint8_t
#define ADDR_INCUBATION_STARTED 10  // uint8_t
#define ADDR_INCUBATION_EPOCH 11    // time_t (4 bytes on ESP8266)


uint8_t eepromHeaterMode = 0;
uint8_t eepromManualState = 0;

// ================= NTP =================

const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 19800;  // IST
const int DAYLIGHT_OFFSET_SEC = 0;

bool timeValid = false;
struct tm timeinfo;  // ✅ GLOBAL


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

  UI_INCUBATION_MENU,
  UI_INCUBATION_START,
  UI_EDIT_START_DATETIME,
  UI_INCUBATION_INFO,
  UI_CONFIRM_START,

  UI_STATUS,
  UI_WIFI_MENU,
  UI_WIFI_STATUS,
  UI_SETTINGS,
  UI_SET_TEMP,
  UI_HEATER_MODE,
  UI_MANUAL_HEATER,
  UI_HYSTERESIS
};


UiState uiState = UI_HOME;

// ================== MENUS ==================
const char* menuItems[] = { "Incubation", "Status", "WiFi", "Settings", "Back" };
const char* incubationMenuItems[] = { "Start", "Info", "Reset", "Back" };
const char* wifiMenuItems[] = { "Connect", "Status", "Back" };
const char* settingsItems[] = { "Set Temperature", "Heater Mode", "Hysteresis", "Back" };
const char* heaterModeItems[] = { "AUTO", "MANUAL" };

int menuIndex = 0;
int wifiMenuIndex = 0;
int settingsIndex = 0;
int heaterModeIndex = 0;
int incubationMenuIndex = 0;
int incubationStartIndex = 0;  // 0=EDIT, 1=START, 2=BACK
int confirmStartIndex = 0;     // 0 = CONFIRM, 1 = CANCEL
uint8_t statusPage = 0;        // 0 = Page 1, 1 = Page 2


// ===== Manual Heater =====
int manualSelectIndex = 0;  // 0=OFF 1=ON

// ================== DATA ==================
float liveHum = 55.0;
float setTemp = 37.5;
float hysteresis = 0.3;
//uint8_t incubationDay = 7;  // STATUS screen (V1 placeholder)

// ================= INCUBATION =================
#define INCUBATION_DAYS 21

bool incubationStarted = false;
time_t incubationStartEpoch = 0;
uint8_t incubationDay = 0;

// -------- Edit start date & time --------
enum EditField {
  EDIT_DAY,
  EDIT_MONTH,
  EDIT_YEAR,
  EDIT_HOUR,
  EDIT_MINUTE,
  EDIT_DONE
};

EditField editField = EDIT_DAY;

uint8_t editDay;
uint8_t editMonth;
uint16_t editYear;
uint8_t editHour;
uint8_t editMinute;


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

  // ✅ ADD THESE
  uint8_t incStarted = 0;
  EEPROM.get(ADDR_INCUBATION_STARTED, incStarted);
  incubationStarted = (incStarted == 1);

  EEPROM.get(ADDR_INCUBATION_EPOCH, incubationStartEpoch);

  updateIncubationDay();  // 🔑 recompute derived value
}

const char* wifiQuality(int rssi) {
  if (rssi >= -55) return "EXCELLENT";
  if (rssi >= -65) return "GOOD";
  if (rssi >= -75) return "FAIR";
  return "POOR";
}

void drawWifiStatus() {
  drawHeader("WIFI STATUS");

  bool connected = (WiFi.status() == WL_CONNECTED);

  display.setCursor(0, 14);
  display.print("State : ");
  display.println(connected ? "CONNECTED" : "DISCONNECTED");

  display.setCursor(0, 24);
  display.print("SSID  : ");
  display.println(connected ? WiFi.SSID() : "--");

  display.setCursor(0, 34);
  display.print("IP    : ");
  if (connected)
    display.println(WiFi.localIP());
  else
    display.println("--");

  display.setCursor(0, 44);
  display.print("RSSI  : ");
  if (connected) {
    int rssi = WiFi.RSSI();
    display.print(rssi);
    display.print(" ");
    display.println(wifiQuality(rssi));
  } else {
    display.println("--");
  }

  display.setCursor(0, 54);
  display.print("Time  : ");
  display.println(timeValid ? "SYNCED" : "NOT SYNCED");

  display.display();
}


void updateIncubationDay() {
  if (!incubationStarted) {
    incubationDay = 0;
    return;
  }

  time_t nowEpoch = time(nullptr);
  time_t elapsed = nowEpoch - incubationStartEpoch;

  if (elapsed < 0) elapsed = 0;  // safety

  uint16_t daysPassed = elapsed / 86400;
  incubationDay = constrain(daysPassed + 1, 1, 21);
}

void saveSettingsToEEPROM() {
  eepromHeaterMode = (uint8_t)heaterMode;
  eepromManualState = manualHeaterOn ? 1 : 0;

  EEPROM.put(ADDR_SET_TEMP, setTemp);
  EEPROM.put(ADDR_HYSTERESIS, hysteresis);
  EEPROM.put(ADDR_HEATER_MODE, eepromHeaterMode);
  EEPROM.put(ADDR_MANUAL_STATE, eepromManualState);

  // ✅ ADD THESE TWO LINES
  EEPROM.put(ADDR_INCUBATION_STARTED, (uint8_t)incubationStarted);
  EEPROM.put(ADDR_INCUBATION_EPOCH, incubationStartEpoch);

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
            timeinfo.tm_min);

    display.setCursor(90, 0);
    display.print(timeStr);
  }


  display.display();
}

void drawConfirmStart() {
  drawHeader("CONFIRM START");

  display.setCursor(0, 20);
  display.printf("%02d-%02d-%04d %02d:%02d",
                 editDay, editMonth, editYear,
                 editHour, editMinute);

  display.setCursor(0, 44);
  display.print(confirmStartIndex == 0 ? "> " : "  ");
  display.println("CONFIRM");

  display.print(confirmStartIndex == 1 ? "> " : "  ");
  display.println("CANCEL");

  display.display();
}



void drawMenu() {
  drawHeader("MAIN MENU");
  for (int i = 0; i < 5; i++) {
    display.setCursor(0, 14 + i * 10);
    display.print(i == menuIndex ? "> " : "  ");
    display.println(menuItems[i]);
  }
  display.display();
}

void drawIncubationMenu() {
  drawHeader("INCUBATION");
  for (int i = 0; i < 4; i++) {
    display.setCursor(0, 14 + i * 10);
    display.print(i == incubationMenuIndex ? "> " : "  ");
    display.println(incubationMenuItems[i]);
  }
  display.display();
}

void drawIncubationStart() {
  drawHeader("START INCUBATION");

  display.setCursor(0, 16);
  display.print("Date : ");
  display.printf("%02d-%02d-%04d",
                 editDay, editMonth, editYear);

  display.setCursor(0, 26);
  display.print("Time : ");
  display.printf("%02d:%02d", editHour, editMinute);

  display.setCursor(0, 44);
  display.print(incubationStartIndex == 0 ? "> " : "  ");
  display.println("EDIT");

  display.print(incubationStartIndex == 1 ? "> " : "  ");
  display.println("START");

  display.print(incubationStartIndex == 2 ? "> " : "  ");
  display.println("BACK");

  display.display();
}
void drawIncubationInfo() {
  drawHeader("INCUBATION INFO");

  if (!incubationStarted) {
    display.setCursor(0, 24);
    display.println("Not started");
    display.display();
    return;
  }

  // Convert start epoch to date
  struct tm startTm;
  localtime_r(&incubationStartEpoch, &startTm);

  uint8_t sDay = startTm.tm_mday;
  uint8_t sMonth = startTm.tm_mon + 1;
  uint16_t sYear = startTm.tm_year + 1900;

  // Start date
  display.setCursor(0, 14);
  display.printf("Start : %02d-%02d-%04d",
                 sDay, sMonth, sYear);

  // Current day
  display.setCursor(0, 26);
  display.printf("Day   : %02d / 21", incubationDay);

  // Milestones
  display.setCursor(0, 38);
  display.print("7d  : ");
  display.println(incubationDay >= 7 ? "DONE" : "PENDING");

  display.setCursor(0, 48);
  display.print("14d : ");
  display.println(incubationDay >= 14 ? "DONE" : "PENDING");

  display.setCursor(0, 58);
  display.print("21d : ");
  display.println(incubationDay >= 21 ? "DONE" : "PENDING");

  display.display();
}

void drawEditStartDateTime() {
  drawHeader("EDIT START TIME");

  display.setCursor(0, 14);
  if (editField == EDIT_DAY)
    display.printf("Day   : [%02d]", editDay);
  else
    display.printf("Day   :  %02d ", editDay);

  display.setCursor(0, 24);
  if (editField == EDIT_MONTH)
    display.printf("Month : [%02d]", editMonth);
  else
    display.printf("Month :  %02d ", editMonth);

  display.setCursor(0, 34);
  if (editField == EDIT_YEAR)
    display.printf("Year  : [%04d]", editYear);
  else
    display.printf("Year  :  %04d ", editYear);

  display.setCursor(0, 44);
  if (editField == EDIT_HOUR)
    display.printf("Hour  : [%02d]", editHour);
  else
    display.printf("Hour  :  %02d ", editHour);

  display.setCursor(0, 54);
  if (editField == EDIT_MINUTE)
    display.printf("Min   : [%02d]", editMinute);
  else
    display.printf("Min   :  %02d ", editMinute);

  display.display();
}

void drawStatusPage1() {
  drawHeader("STATUS (1/2)");

  float delta = liveTemp - setTemp;

  display.setCursor(0, 14);
  display.print("Temp : ");
  if (sensorValid) {
    display.print(liveTemp, 1);
    display.print((char)247);
    display.print("C ");

    display.print("(");
    display.print(delta >= 0 ? "+" : "");
    display.print(delta, 1);
    display.print(")");
  } else {
    display.print("SENSOR ERR");
  }

  display.setCursor(0, 24);
  display.print("Hum  : -- %");

  display.setCursor(0, 34);
  display.print("Day  : ");
  if (incubationStarted) {
    if (incubationDay < 10) display.print("0");
    display.print(incubationDay);
    display.print(" / 21");
  } else {
    display.print("--");
  }

  display.setCursor(0, 44);
  display.print("Heater: ");
  display.print(heaterOn ? "ON " : "OFF");
  display.print(" ");
  display.print("Mode: ");
  display.print(heaterMode == HEATER_AUTO ? "AUT" : "MAN");

  display.setCursor(0, 54);
  display.print("Set  : ");
  display.print(setTemp, 1);
  display.print(" Hys : ");
  display.print(hysteresis, 1);

  display.display();
}
void drawStatusPage2() {
  drawHeader("STATUS (2/2)");

  // Remaining days
  display.setCursor(0, 14);
  display.print("Left  : ");
  if (incubationStarted) {
    int left = INCUBATION_DAYS - incubationDay;
    if (left < 0) left = 0;
    display.print(left);
    display.print(" days");
  } else {
    display.print("--");
  }

  // Hatch date
  display.setCursor(0, 24);
  display.print("Hatch : ");
  if (incubationStarted) {
    time_t hatchEpoch = incubationStartEpoch + (INCUBATION_DAYS * 86400UL);
    struct tm hatchTm;
    localtime_r(&hatchEpoch, &hatchTm);

    const char* months[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    display.printf("%02d-%s",
                   hatchTm.tm_mday,
                   months[hatchTm.tm_mon]);
  } else {
    display.print("--");
  }

  // Alarm placeholder
  display.setCursor(0, 36);
  display.print("Alarm : ");
  display.print(sensorValid ? "NONE" : "SENSOR");

  // Log placeholder
  display.setCursor(0, 48);
  display.print("Log   : OK");

  display.display();
}
void drawStatus() {
  if (statusPage == 0)
    drawStatusPage1();
  else
    drawStatusPage2();
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

void startIncubationFromEdit() {
  struct tm t;
  t.tm_mday = editDay;
  t.tm_mon = editMonth - 1;
  t.tm_year = editYear - 1900;
  t.tm_hour = editHour;
  t.tm_min = editMinute;
  t.tm_sec = 0;

  incubationStartEpoch = mktime(&t);
  incubationStarted = true;

  updateIncubationDay();  // ✅ ADD THIS LINE
  saveSettingsToEEPROM();
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

void loop() {
  button.loop();
  temperatureTask();

  static unsigned long lastDayUpdate = 0;
  if (millis() - lastDayUpdate > 1000) {
    lastDayUpdate = millis();
    updateIncubationDay();
  }


  // ---------- NTP update ----------
  static unsigned long lastTimeCheck = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastTimeCheck > 1000) {
    lastTimeCheck = millis();
    timeValid = getLocalTime(&timeinfo);
  }

  // ---------- Heater + screen refresh ----------
  if (millis() - lastHeaterUpdate > HEATER_INTERVAL) {
    lastHeaterUpdate = millis();
    updateHeaterControl();

    if (uiState == UI_HOME) drawHome();
    else if (uiState == UI_STATUS) drawStatus();
  }

  // ---------- Encoder ----------
  int enc = readEncoder();
  if (enc != 0) lastUiActivity = millis();

  if (enc != 0) {

    // ===== MAIN MENU =====
    if (uiState == UI_MENU) {
      menuIndex = (menuIndex + enc + 5) % 5;
      drawMenu();
    }

    // ===== INCUBATION MENU =====
    else if (uiState == UI_INCUBATION_MENU) {
      incubationMenuIndex = (incubationMenuIndex + enc + 4) % 4;
      drawIncubationMenu();
    }

    // ===== INCUBATION START SCREEN (EDIT / START / BACK) =====
    else if (uiState == UI_INCUBATION_START) {
      incubationStartIndex = (incubationStartIndex + enc + 3) % 3;
      drawIncubationStart();
    }

    else if (uiState == UI_CONFIRM_START) {
      confirmStartIndex = (confirmStartIndex + enc + 2) % 2;
      drawConfirmStart();
    }

    // ===== EDIT DATE & TIME (LINEAR EDIT) =====
    else if (uiState == UI_EDIT_START_DATETIME) {
      switch (editField) {
        case EDIT_DAY:
          editDay = constrain(editDay + enc, 1, 31);
          break;
        case EDIT_MONTH:
          editMonth = constrain(editMonth + enc, 1, 12);
          break;
        case EDIT_YEAR:
          editYear = constrain(editYear + enc, 2024, 2035);
          break;
        case EDIT_HOUR:
          editHour = constrain(editHour + enc, 0, 23);
          break;
        case EDIT_MINUTE:
          editMinute = constrain(editMinute + enc, 0, 59);
          break;
        default:
          break;
      }
      drawEditStartDateTime();
    } else if (uiState == UI_STATUS) {
      statusPage = (statusPage + enc + 2) % 2;
      drawStatus();
    }


    // ===== WIFI MENU =====
    else if (uiState == UI_WIFI_MENU) {
      wifiMenuIndex = (wifiMenuIndex + enc + 3) % 3;
      drawWifiMenu();
    }

    // ===== SETTINGS =====
    else if (uiState == UI_SETTINGS) {
      settingsIndex = (settingsIndex + enc + 4) % 4;
      drawSettings();
    }

    // ===== SET TEMP =====
    else if (uiState == UI_SET_TEMP) {
      setTemp = constrain(setTemp + enc * 0.1, 30.0, 40.0);
      drawSetTemp();
    }

    // ===== HEATER MODE =====
    else if (uiState == UI_HEATER_MODE) {
      heaterModeIndex = (heaterModeIndex + enc + 2) % 2;
      drawHeaterMode();
    }

    // ===== MANUAL HEATER =====
    else if (uiState == UI_MANUAL_HEATER) {
      manualSelectIndex = (manualSelectIndex + enc + 2) % 2;
      drawManualHeater();
    }

    // ===== HYSTERESIS =====
    else if (uiState == UI_HYSTERESIS) {
      hysteresis = constrain(hysteresis + enc * 0.1, 0.1, 1.0);
      drawHysteresis();
    }
  }

  // ---------- Button ----------
  if (button.isPressed()) {
    lastUiActivity = millis();

    // ===== HOME =====
    if (uiState == UI_HOME) {
      uiState = UI_MENU;
      drawMenu();
    }

    // ===== MAIN MENU =====
    else if (uiState == UI_MENU) {
      if (menuIndex == 0) {
        incubationMenuIndex = 0;
        uiState = UI_INCUBATION_MENU;
        drawIncubationMenu();
      } else if (menuIndex == 1) {
        uiState = UI_STATUS;
        drawStatus();
      } else if (menuIndex == 2) {
        wifiMenuIndex = 0;
        uiState = UI_WIFI_MENU;
        drawWifiMenu();
      } else if (menuIndex == 3) {
        settingsIndex = 0;
        uiState = UI_SETTINGS;
        drawSettings();
      } else {
        uiState = UI_HOME;
        drawHome();
      }
    }

    // ===== INCUBATION MENU =====
    else if (uiState == UI_INCUBATION_MENU) {
      if (incubationMenuIndex == 0) {  // Start
        editDay = timeinfo.tm_mday;
        editMonth = timeinfo.tm_mon + 1;
        editYear = timeinfo.tm_year + 1900;
        editHour = timeinfo.tm_hour;
        editMinute = timeinfo.tm_min;

        incubationStartIndex = 0;
        uiState = UI_INCUBATION_START;
        drawIncubationStart();
      } else if (incubationMenuIndex == 1) {  // Info
        uiState = UI_INCUBATION_INFO;
        drawIncubationInfo();
      } else if (incubationMenuIndex == 2) {  // Reset
        incubationStarted = false;
        incubationDay = 0;
        saveSettingsToEEPROM();
        drawIncubationMenu();
      } else {
        uiState = UI_MENU;
        drawMenu();
      }
    }

    // ===== INCUBATION START =====
    else if (uiState == UI_INCUBATION_START) {

      if (incubationStartIndex == 0) {  // EDIT
        editField = EDIT_DAY;
        uiState = UI_EDIT_START_DATETIME;
        drawEditStartDateTime();
      }

      else if (incubationStartIndex == 1) {  // START
        confirmStartIndex = 0;               // default to CONFIRM
        uiState = UI_CONFIRM_START;          // ✅ GO TO CONFIRM SCREEN
        drawConfirmStart();
      }

      else {  // BACK
        uiState = UI_INCUBATION_MENU;
        drawIncubationMenu();
      }
    }


    // ===== EDIT DATE & TIME =====
    else if (uiState == UI_EDIT_START_DATETIME) {
      editField = (EditField)(editField + 1);
      if (editField == EDIT_DONE) {
        uiState = UI_CONFIRM_START;
        drawConfirmStart();
      } else {
        drawEditStartDateTime();
      }
    }
    // ===== CONFIRM START =====
    else if (uiState == UI_CONFIRM_START) {
      if (confirmStartIndex == 0) {  // CONFIRM
        startIncubationFromEdit();
        updateIncubationDay();  // ✅ SAFETY
        uiState = UI_INCUBATION_INFO;
        drawIncubationInfo();
      } else {
        uiState = UI_INCUBATION_START;
        drawIncubationStart();
      }
    } else if (uiState == UI_INCUBATION_INFO) {
      uiState = UI_INCUBATION_MENU;
      drawIncubationMenu();
    }
    // ===== STATUS =====
    else if (uiState == UI_STATUS) {
      statusPage = 0;  // reset to page 1
      uiState = UI_MENU;
      drawMenu();
    }

    // ===== WIFI MENU =====
    else if (uiState == UI_WIFI_MENU) {
      if (wifiMenuIndex == 0) {  // Connect
        wm.startConfigPortal("EggIncubator_Setup");
        drawWifiMenu();
      } else if (wifiMenuIndex == 1) {  // WiFi Status
        lastUiActivity = millis();
        uiState = UI_WIFI_STATUS;
        drawWifiStatus();
      } else {  // Back
        uiState = UI_MENU;
        drawMenu();
      }
    } else if (uiState == UI_WIFI_STATUS) {
      uiState = UI_WIFI_MENU;
      drawWifiMenu();
    }


    // ===== SETTINGS =====
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

    // ===== HEATER MODE =====
    else if (uiState == UI_HEATER_MODE) {
      heaterMode = heaterModeIndex == 0 ? HEATER_AUTO : HEATER_MANUAL;
      saveSettingsToEEPROM();

      if (heaterMode == HEATER_MANUAL) {
        manualSelectIndex = manualHeaterOn ? 1 : 0;
        uiState = UI_MANUAL_HEATER;
        drawManualHeater();
      } else {
        uiState = UI_SETTINGS;
        drawSettings();
      }
    }

    // ===== MANUAL HEATER =====
    else if (uiState == UI_MANUAL_HEATER) {
      manualHeaterOn = (manualSelectIndex == 1);
      saveSettingsToEEPROM();
      uiState = UI_SETTINGS;
      drawSettings();
    }

    // ===== SET TEMP =====
    else if (uiState == UI_SET_TEMP) {
      saveSettingsToEEPROM();
      uiState = UI_SETTINGS;
      drawSettings();
    }

    // ===== HYSTERESIS =====
    else if (uiState == UI_HYSTERESIS) {
      saveSettingsToEEPROM();
      uiState = UI_SETTINGS;
      drawSettings();
    }
  }

  // ---------- UI timeout (protect edit screens) ----------
  // ---------- UI timeout (protect edit & info screens) ----------
  if (uiState != UI_HOME && uiState != UI_EDIT_START_DATETIME && uiState != UI_CONFIRM_START && uiState != UI_WIFI_STATUS &&  // ✅ ADD THIS LINE
      millis() - lastUiActivity > UI_TIMEOUT) {

    uiState = UI_HOME;
    drawHome();
  }
}
