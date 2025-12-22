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
#define ADDR_MAX_SAFE_TEMP 15       // float (4 bytes)
#define ADDR_MIN_SAFE_TEMP 19       // float (4 bytes)



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
  UI_HEATER_MODE,
  UI_MANUAL_HEATER,
  UI_HYSTERESIS,
  UI_EDIT_TEMPERATURE,
  UI_CONFIRM_TEMPERATURE,
};
UiState uiState = UI_HOME;

enum AlarmType {
  ALARM_NONE,
  ALARM_SENSOR_FAULT,
  ALARM_OVER_TEMP,
  ALARM_UNDER_TEMP
};
AlarmType activeAlarm = ALARM_NONE;

enum TempEditField {
  EDIT_TARGET_TEMP,
  EDIT_MAX_SAFE_TEMP,
  EDIT_MIN_SAFE_TEMP,
  EDIT_TEMP_DONE
};

TempEditField tempEditField = EDIT_TARGET_TEMP;


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
int incubationStartIndex = 0;                   // 0=EDIT, 1=START, 2=BACK
int confirmStartIndex = 0;                      // 0 = CONFIRM, 1 = CANCEL
uint8_t statusPage = 0;                         // 0 = Page 1, 1 = Page 2
const unsigned long UI_REFRESH_INTERVAL = 500;  // ms
unsigned long lastUiRefresh = 0;

// ===== Manual Heater =====
int manualSelectIndex = 0;  // 0=OFF 1=ON

// ================== DATA ==================
float liveHum = 55.0;
float setTemp = 37.5;
float hysteresis = 0.3;
//uint8_t incubationDay = 7;  // STATUS screen (V1 placeholder)
float maxSafeTemp = 39.5;
float minSafeTemp = 35.0;
float editTargetTemp;
float editMaxSafeTemp;
float editMinSafeTemp;




// ================= INCUBATION =================
#define INCUBATION_DAYS 21

bool incubationStarted = false;
uint32_t incubationStartEpoch = 0;
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
const unsigned long MIN_RELAY_TIME = 5000;  // 5 seconds
unsigned long lastRelayChange = 0;


// ================== WIFI ==================
WiFiManager wm;

// ================= TEMPERATURE TASK =================
void temperatureTask() {
  unsigned long now = millis();

  // ---------- Request temperature ----------
  if (!tempRequested && now - lastTempRequest >= TEMP_INTERVAL) {
    sensors.requestTemperatures();
    tempRequestTime = now;
    tempRequested = true;
    lastTempRequest = now;
  }

  // ---------- Read temperature ----------
  if (tempRequested && now - tempRequestTime >= 750) {
    float t = sensors.getTempCByIndex(0);

    if (t != DEVICE_DISCONNECTED_C && t > -40.0 && t < 100.0) {
      rawTemp = t;

      // 🔑 Proper filter initialization
      if (!sensorValid) {
        filteredTemp = rawTemp;  // first valid sample
      } else {
        filteredTemp = (filteredTemp * 0.7f) + (rawTemp * 0.3f);
      }

      liveTemp = filteredTemp;
      sensorValid = true;
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

  bool desiredState = heaterOn;

  if (heaterMode == HEATER_AUTO) {
    if (liveTemp <= setTemp - hysteresis)
      desiredState = true;
    else if (liveTemp >= setTemp + hysteresis)
      desiredState = false;
  } else {
    desiredState = manualHeaterOn;
  }

  unsigned long now = millis();

  // 🔒 Anti-chatter protection
  if (desiredState != heaterOn && (now - lastRelayChange) >= MIN_RELAY_TIME) {

    heaterOn = desiredState;
    digitalWrite(HEATER_PIN, heaterOn ? HIGH : LOW);
    lastRelayChange = now;
  }
}
void updateAlarms() {
  if (!sensorValid) {
    activeAlarm = ALARM_SENSOR_FAULT;
    return;
  }

  if (liveTemp >= maxSafeTemp) {
    activeAlarm = ALARM_OVER_TEMP;
    return;
  }

  if (liveTemp <= minSafeTemp) {
    activeAlarm = ALARM_UNDER_TEMP;
    return;
  }

  activeAlarm = ALARM_NONE;
}


/**
 * @brief Load all persisted user and system settings from EEPROM
 *
 * This function restores configuration values such as temperature setpoints,
 * heater mode, safety limits, and incubation state after a reboot or power loss.
 *
 * It also:
 *  - Validates all EEPROM-loaded values
 *  - Applies safe defaults if data is corrupted or out of range
 *  - Ensures logical constraints (e.g. min < max temperature)
 *  - Recomputes derived runtime values (incubation day)
 */
void loadSettingsFromEEPROM() {

  /* =========================================================
   * EEPROM INITIALIZATION
   * ========================================================= */
  EEPROM.begin(EEPROM_SIZE);


  /* =========================================================
   * LOAD USER CONFIGURATION
   * ========================================================= */
  EEPROM.get(ADDR_SET_TEMP, setTemp);
  EEPROM.get(ADDR_HYSTERESIS, hysteresis);

  EEPROM.get(ADDR_HEATER_MODE, eepromHeaterMode);
  EEPROM.get(ADDR_MANUAL_STATE, eepromManualState);

  EEPROM.get(ADDR_MAX_SAFE_TEMP, maxSafeTemp);
  EEPROM.get(ADDR_MIN_SAFE_TEMP, minSafeTemp);


  /* =========================================================
   * SAFETY LIMIT VALIDATION (CRITICAL)
   * ========================================================= */

  // Validate MAX safe temperature (upper alarm threshold)
  if (maxSafeTemp < 38.0 || maxSafeTemp > 42.0)
    maxSafeTemp = 39.5;

  // Validate MIN safe temperature (lower alarm threshold)
  if (minSafeTemp < 30.0 || minSafeTemp > 37.0)
    minSafeTemp = 35.0;

  // Ensure logical ordering: min must be at least 0.5°C below max
  if (minSafeTemp >= maxSafeTemp - 0.5)
    minSafeTemp = maxSafeTemp - 0.5;


  /* =========================================================
   * CONTROL SETPOINT VALIDATION
   * ========================================================= */

  // Validate incubation temperature setpoint
  if (setTemp < 30.0 || setTemp > 40.0)
    setTemp = 37.5;

  // Validate hysteresis band
  if (hysteresis < 0.1 || hysteresis > 1.0)
    hysteresis = 0.3;


  /* =========================================================
   * HEATER MODE RESTORATION
   * ========================================================= */

  // Heater mode: 0 = AUTO, 1 = MANUAL
  if (eepromHeaterMode > 1)
    eepromHeaterMode = 0;

  heaterMode = (HeaterMode)eepromHeaterMode;

  // Manual heater ON/OFF state
  manualHeaterOn = (eepromManualState == 1);


  /* =========================================================
   * INCUBATION STATE RESTORATION
   * ========================================================= */

  uint8_t incStarted = 0;
  EEPROM.get(ADDR_INCUBATION_STARTED, incStarted);
  incubationStarted = (incStarted == 1);

  // Load stored incubation start epoch (Unix time)
  EEPROM.get(ADDR_INCUBATION_EPOCH, incubationStartEpoch);


  /* =========================================================
   * DERIVED RUNTIME CALCULATIONS
   * ========================================================= */

  // Recompute current incubation day based on epoch
  updateIncubationDay();
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
  if (!incubationStarted || !timeValid) {
    incubationDay = 0;
    return;
  }

  uint32_t nowEpoch = (uint32_t)time(nullptr);
  time_t elapsed = nowEpoch - incubationStartEpoch;

  if (elapsed < 0) elapsed = 0;

  uint16_t daysPassed = elapsed / 86400;
  incubationDay = constrain(daysPassed + 1, 1, INCUBATION_DAYS);
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

  EEPROM.put(ADDR_MAX_SAFE_TEMP, maxSafeTemp);
  EEPROM.put(ADDR_MIN_SAFE_TEMP, minSafeTemp);


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
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // ---------- Date & Time (Top Left) ----------
  if (timeValid) {
    struct tm t = timeinfo;

    const char* months[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    display.setCursor(0, 0);
    display.printf("%02d-%s  %02d:%02d",
                   t.tm_mday,
                   months[t.tm_mon],
                   t.tm_hour,
                   t.tm_min);
  } else {
    display.setCursor(0, 0);
    display.print("-- ---  --:--");
  }

  // ---------- WiFi Signal Bars (Top Right) ----------
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    int bars = 0;

    if (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else if (rssi > -85) bars = 1;

    int x = 110;
    for (int i = 0; i < bars; i++) {
      display.fillRect(x + i * 4, 8 - i * 2, 3, i * 2 + 2, SSD1306_WHITE);
    }
  }
  // ---------- HEADER SEPARATOR LINE (ADD THIS) ----------
  display.drawLine(0, 12, 127, 12, SSD1306_WHITE);
  // ---------- Temperature & Humidity ----------
  display.setCursor(0, 16);
  display.print("T : ");
  if (sensorValid) {
    display.print(liveTemp, 1);
    display.print((char)247);
    display.print("C");
  } else {
    display.print("SENSOR ERR");
  }

  display.setCursor(80, 16);
  display.print("H :--%");

  // ---------- Heater Status ----------
  display.setCursor(0, 28);
  display.print("Heater: ");
  display.print(heaterOn ? "ON " : "OFF");
  display.print(" ");
  display.print(heaterMode == HEATER_AUTO ? "AUTO" : "MAN");

  // ---------- Incubation Day ----------
  display.setCursor(0, 40);
  display.print("Day  : ");
  if (incubationStarted) {
    if (incubationDay < 10) display.print("0");
    display.print(incubationDay);
    display.print(" / 21");
  } else {
    display.print("--");
  }

  // ---------- Hatch Date ----------
  display.setCursor(0, 52);
  display.print("Hatch: ");
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
  time_t t = (time_t)incubationStartEpoch;  // 🔑 convert safely
  localtime_r(&t, &startTm);


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

  switch (activeAlarm) {
    case ALARM_NONE: display.print("NONE"); break;
    case ALARM_SENSOR_FAULT: display.print("SENSOR"); break;
    case ALARM_OVER_TEMP: display.print("OVER TEMP"); break;
    case ALARM_UNDER_TEMP: display.print("UNDER TEMP"); break;
  }

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

void drawEditTemperature() {
  drawHeader("EDIT TEMPERATURE");

  display.setCursor(0, 18);
  display.print(tempEditField == EDIT_TARGET_TEMP ? "> " : "  ");
  display.printf("Target : %.1f C", editTargetTemp);

  display.setCursor(0, 30);
  display.print(tempEditField == EDIT_MAX_SAFE_TEMP ? "> " : "  ");
  display.printf("MaxSafe: %.1f C", editMaxSafeTemp);

  display.setCursor(0, 42);
  display.print(tempEditField == EDIT_MIN_SAFE_TEMP ? "> " : "  ");
  display.printf("MinSafe: %.1f C", editMinSafeTemp);

  display.display();
}

void drawConfirmTemperature() {
  drawHeader("CONFIRM TEMP");

  display.setCursor(0, 22);
  display.printf("Target: %.1f C", editTargetTemp);

  display.setCursor(0, 34);
  display.printf("Safe  : %.1f - %.1f C",
                 editMinSafeTemp,
                 editMaxSafeTemp);

  display.setCursor(0, 48);
  display.print(confirmStartIndex == 0 ? "> CONFIRM" : "  CONFIRM");

  display.setCursor(0, 58);
  display.print(confirmStartIndex == 1 ? "> CANCEL" : "  CANCEL");

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
  struct tm t = {};  // 🔒 ZERO INITIALIZE

  t.tm_mday = editDay;
  t.tm_mon = editMonth - 1;
  t.tm_year = editYear - 1900;
  t.tm_hour = editHour;
  t.tm_min = editMinute;
  t.tm_sec = 0;
  t.tm_isdst = -1;  // 🔑 let system decide DST

  time_t epoch = mktime(&t);

  // 🔒 Reject invalid epoch
  if (epoch < 1700000000UL || epoch > 2200000000UL) {
    incubationStarted = false;
    return;
  }

  incubationStartEpoch = epoch;
  incubationStarted = true;

  updateIncubationDay();
  saveSettingsToEEPROM();
}



// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(CLK_PIN, INPUT);
  pinMode(DT_PIN, INPUT);
  pinMode(SW_PIN, INPUT);
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);
  heaterOn = false;
  manualHeaterOn = false;



  button.setDebounceTime(50);

  Wire.begin(D2, D1);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(false);

  loadSettingsFromEEPROM();
  Serial.println("EEPROM DEBUG");
  Serial.print("Started : ");
  Serial.println(incubationStarted);
  Serial.print("Epoch   : ");
  Serial.println(incubationStartEpoch);


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
    updateAlarms();
  }

  unsigned long now = millis();

  if (now - lastUiRefresh > UI_REFRESH_INTERVAL) {
    lastUiRefresh = now;

    if (uiState == UI_HOME)
      drawHome();
    else if (uiState == UI_STATUS)
      drawStatus();
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
    } else if (uiState == UI_EDIT_TEMPERATURE) {

      if (tempEditField == EDIT_TARGET_TEMP) {
        editTargetTemp = constrain(editTargetTemp + enc * 0.1, 30.0, 40.0);
      } else if (tempEditField == EDIT_MAX_SAFE_TEMP) {
        editMaxSafeTemp = constrain(editMaxSafeTemp + enc * 0.1, 38.0, 42.0);
      } else if (tempEditField == EDIT_MIN_SAFE_TEMP) {
        editMinSafeTemp = constrain(editMinSafeTemp + enc * 0.1, 30.0, 37.0);
      }

      // 🔒 Safety rules (enforced live)
      if (editMinSafeTemp >= editMaxSafeTemp - 0.5)
        editMinSafeTemp = editMaxSafeTemp - 0.5;

      if (editTargetTemp >= editMaxSafeTemp)
        editTargetTemp = editMaxSafeTemp - 0.2;

      if (editTargetTemp <= editMinSafeTemp)
        editTargetTemp = editMinSafeTemp + 0.2;

      drawEditTemperature();
    } else if (uiState == UI_CONFIRM_TEMPERATURE) {
      confirmStartIndex = (confirmStartIndex + enc + 2) % 2;
      drawConfirmTemperature();
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

        if (!timeValid) {
          // 🔒 Safe defaults if NTP not ready
          editDay = 1;
          editMonth = 1;
          editYear = 2025;
          editHour = 0;
          editMinute = 0;
        } else {
          editDay = timeinfo.tm_mday;
          editMonth = timeinfo.tm_mon + 1;
          editYear = timeinfo.tm_year + 1900;
          editHour = timeinfo.tm_hour;
          editMinute = timeinfo.tm_min;
        }

        incubationStartIndex = 0;
        uiState = UI_INCUBATION_START;
        drawIncubationStart();
      } else if (incubationMenuIndex == 1) {  // Info
        uiState = UI_INCUBATION_INFO;
        drawIncubationInfo();
      } else if (incubationMenuIndex == 2) {  // Reset
        incubationStarted = false;
        incubationDay = 0;
        incubationStartEpoch = 0;  // 🔒 CLEAR STORED TIME
        saveSettingsToEEPROM();
        drawIncubationMenu();
      }

      else {
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
        editTargetTemp = setTemp;
        editMaxSafeTemp = maxSafeTemp;
        editMinSafeTemp = minSafeTemp;

        tempEditField = EDIT_TARGET_TEMP;
        uiState = UI_EDIT_TEMPERATURE;
        drawEditTemperature();
      } else if (settingsIndex == 1) {
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

    // ===== EDIT TEMPERATURE =====
    else if (uiState == UI_EDIT_TEMPERATURE) {

      tempEditField = (TempEditField)(tempEditField + 1);

      if (tempEditField == EDIT_TEMP_DONE) {
        confirmStartIndex = 0;
        uiState = UI_CONFIRM_TEMPERATURE;
        drawConfirmTemperature();
      } else {
        drawEditTemperature();
      }
    }

    // ===== CONFIRM TEMPERATURE =====
    else if (uiState == UI_CONFIRM_TEMPERATURE) {

      if (confirmStartIndex == 0) {  // CONFIRM
        setTemp = editTargetTemp;
        maxSafeTemp = editMaxSafeTemp;
        minSafeTemp = editMinSafeTemp;
        saveSettingsToEEPROM();
      }

      uiState = UI_SETTINGS;
      drawSettings();
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

    // ===== HYSTERESIS =====
    else if (uiState == UI_HYSTERESIS) {
      saveSettingsToEEPROM();
      uiState = UI_SETTINGS;
      drawSettings();
    }
  }

  // ---------- UI timeout (protect edit screens) ----------
  // ---------- UI timeout (protect edit & info screens) ----------
  if (uiState != UI_HOME && uiState != UI_EDIT_START_DATETIME && uiState != UI_CONFIRM_START && uiState != UI_WIFI_STATUS && uiState != UI_EDIT_TEMPERATURE && uiState != UI_CONFIRM_TEMPERATURE &&  // ✅ ADD THIS LINE
      millis() - lastUiActivity > UI_TIMEOUT) {

    uiState = UI_HOME;
    drawHome();
  }
}
