#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>

// ---------------- WiFi ----------------
const char* ssid = "ST - LAB";
const char* password = "simtestlab";

// ---------------- Google Apps Script ----------------
const char* serverName =
"https://script.google.com/macros/s/AKfycbwejNTJ6hRByPLzRMpkq34dBZomUEnfpHsP6yL574k8EPttZohjJcwGAng4CsyE54dL/exec";

// ---------------- NTP ----------------
const char* ntpServer = "time.google.com";
const long gmtOffset_sec = 19800;   // IST
const int daylightOffset_sec = 0;

// ---------------- DS18B20 ----------------
#define ONE_WIRE_BUS 4   // D2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ---------------- Alarm ----------------
#define BUZZER_PIN  D5
#define ACK_BUTTON  D6

bool alarmActive = false;
bool rotationDone[3] = {false, false, false};

// Rotation schedule
int rotHour[3] = {6, 14, 22};
int rotMin[3]  = {0, 0, 0};

// ---------------- Periodic logging ----------------
const unsigned long LOG_INTERVAL = 30000; // 30 seconds
static unsigned long lastLogTime = 0;

// ---------------- Logging helpers ----------------
void sendToSheet(float temp, const char* event) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url = String(serverName) +
               "?temp=" + String(temp, 2) +
               "&hum=52" +
               "&heater=1" +
               "&fan=1" +
               "&volt=4.98" +
               "&event=" + event;

  http.begin(client, url);
  int code = http.GET();
  http.end();

  Serial.print("Sheet log (");
  Serial.print(event);
  Serial.print(") HTTP: ");
  Serial.println(code);
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(ACK_BUTTON, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);

  sensors.begin();

  // WiFi
  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  // NTP (non-blocking safe)
  Serial.print("Syncing time");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  unsigned long startAttempt = millis();
  while (time(nullptr) < 100000 && millis() - startAttempt < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (time(nullptr) < 100000)
    Serial.println("\nNTP sync failed, continuing");
  else
    Serial.println("\nTime synced");
}

// ---------------- Loop ----------------
void loop() {

  // -------- Time --------
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);

  int hour = t->tm_hour;
  int minute = t->tm_min;

  // -------- Temperature --------
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  if (tempC == DEVICE_DISCONNECTED_C) return;

  // -------- Egg rotation alarm --------
  for (int i = 0; i < 3; i++) {
    if (hour == rotHour[i] && minute == rotMin[i] && !rotationDone[i]) {
      rotationDone[i] = true;
      alarmActive = true;
      digitalWrite(BUZZER_PIN, HIGH);
      sendToSheet(tempC, "ROTATION_ALARM");
    }
  }

  // -------- ACK button --------
  if (alarmActive && digitalRead(ACK_BUTTON) == LOW) {
    alarmActive = false;
    digitalWrite(BUZZER_PIN, LOW);
    sendToSheet(tempC, "ROTATION_ACK");
    delay(400); // debounce
  }

  // -------- Reset rotation flags at midnight --------
  if (hour == 0 && minute == 0) {
    rotationDone[0] = rotationDone[1] = rotationDone[2] = false;
  }

  // -------- Periodic process logging (30s) --------
  unsigned long nowMillis = millis();
  if (nowMillis - lastLogTime >= LOG_INTERVAL) {
    lastLogTime = nowMillis;
    sendToSheet(tempC, "");
    Serial.println("Periodic process log");
  }

  delay(500);
}
