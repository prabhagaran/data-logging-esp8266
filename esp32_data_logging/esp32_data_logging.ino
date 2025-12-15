#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>

// ---------------- Google Apps Script ----------------
const char* serverName =
"https://script.google.com/macros/s/AKfycbwfNADzgi649sNlgOOH00oLqstMAYiCaA8G9XDifHp8nUfh7wDxizAEkN4SpWjxk4j1/exec";

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

// Rotation times (HH:MM)
int rotHour[3] = {6, 14, 22};
int rotMin[3]  = {0, 0, 0};

// ---------------- Periodic logging ----------------
const unsigned long LOG_INTERVAL = 30000; // 30 seconds
unsigned long lastLogTime = 0;

// ---------------- Google Sheet DEBUG logger ----------------
void sendToSheet(float temp, const char* event) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi NOT connected, skipping log");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);   // IMPORTANT

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url = String(serverName) +
    "?temp="   + String(temp, 2) +
    "&hum=52" +
    "&heater=ON" +
    "&fan=ON" +
    "&volt=4.98" +
    "&event=" + event;

  Serial.println("--------------------------------------------------");
  Serial.println("Sending to Google Sheet:");
  Serial.println(url);

  if (!http.begin(client, url)) {
    Serial.println("HTTP BEGIN FAILED");
    return;
  }

  int httpCode = http.GET();

  Serial.print("HTTP response code: ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.print("Response payload: ");
    Serial.println(payload);
  } else {
    Serial.print("HTTP error: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(ACK_BUTTON, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);

  sensors.begin();

  // ---------------- WiFiManager ----------------
  WiFiManager wm;

  // Uncomment ONLY to force WiFi reconfiguration
  // wm.resetSettings();

  Serial.println("Starting WiFiManager...");
  bool res = wm.autoConnect("EggIncubator_Setup");

  if (!res) {
    Serial.println("WiFi failed. Restarting...");
    ESP.restart();
  }

  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // ---------------- NTP Sync ----------------
  Serial.print("Syncing time");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  unsigned long startAttempt = millis();
  while (time(nullptr) < 100000 && millis() - startAttempt < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (time(nullptr) < 100000)
    Serial.println("\nNTP sync FAILED, continuing");
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
  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("Temperature sensor disconnected");
    return;
  }

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
    delay(400);
  }

  // -------- Reset at midnight --------
  if (hour == 0 && minute == 0) {
    rotationDone[0] = rotationDone[1] = rotationDone[2] = false;
  }

  // -------- Periodic logging --------
  if (millis() - lastLogTime >= LOG_INTERVAL) {
    lastLogTime = millis();
    sendToSheet(tempC, "");
    Serial.println("Periodic log sent");
  }

  delay(500);
}
