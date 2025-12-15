#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>

// ---------------- WiFi ----------------
const char* ssid = "ST - LAB";
const char* password = "simtestlab";

// ---------------- Google Script ----------------
const char* serverName =
"https://script.google.com/macros/s/AKfycbxkEMndc-Mzh_DZRJUoclMKazBu1skBX_NUfYQven5KQXNH_QMNEQUWVkWQ_3J-y8Wa/exec";

// ---------------- NTP ----------------
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;   // IST
const int daylightOffset_sec = 0;

// ---------------- DS18B20 ----------------
#define ONE_WIRE_BUS 4   // D2 (SAFE)
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Egg Incubator Starting");

  sensors.begin();

  // WiFi
  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  // NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("Syncing time");
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nTime synced");
}

void loop() {

  // -------- Time --------
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  char timeStr[25];
  strftime(timeStr, sizeof(timeStr),
           "%Y-%m-%d %H:%M:%S", timeinfo);

  // -------- Temperature --------
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);

  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20 ERROR");
    delay(10000);
    return;
  }

  // -------- Send to Google Sheets --------
  if (WiFi.status() == WL_CONNECTED) {

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    String url = String(serverName) +
                 "?temp=" + String(tempC, 2);

    http.begin(client, url);
    int httpCode = http.GET();

    Serial.print("Time: ");
    Serial.print(timeStr);
    Serial.print(" | Temp: ");
    Serial.print(tempC, 2);
    Serial.print(" °C | Sheet Status: ");
    Serial.println(httpCode);

    http.end();
  }

  delay(10000);  // every 10 sec
}
