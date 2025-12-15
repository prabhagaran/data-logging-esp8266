#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_BUS 2  // DS18B20 data pin
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

const char* ssid = "ST - LAB";
const char* password = "simtestlab";

const char* serverName = "https://script.google.com/macros/s/AKfycbxkEMndc-Mzh_DZRJUoclMKazBu1skBX_NUfYQven5KQXNH_QMNEQUWVkWQ_3J-y8Wa/exec";

void setup() {
  Serial.begin(115200);
  sensors.begin();
  WiFi.begin(ssid, password);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");
}

void loop() {
   sensors.requestTemperatures(); 
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();           // IMPORTANT for HTTPS
  // Ask sensor to measure
  
    float temperature = sensors.getTempCByIndex(0);
    float humidity = 65.2;
    float voltage = 3.72;

    String url = String(serverName) + "?temp=" + temperature + "&hum=" + humidity + "&volt=" + voltage;

    http.begin(client, url);
    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
      Serial.println("Data sent successfully");
    } else {
      Serial.println("Error sending data");
    }

    http.end();
  }

  delay(10000);  // send every 10 seconds
}
