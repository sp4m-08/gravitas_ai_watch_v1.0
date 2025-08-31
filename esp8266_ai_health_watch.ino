#include <Wire.h>
#include "MAX30100_PulseOximeter.h"
#include <Adafruit_BMP280.h>
#include "OakOLED.h"
#include <MPU6050.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

#define REPORTING_PERIOD_MS 10000

PulseOximeter pox;
Adafruit_BMP280 bmp;
OakOLED oled;
MPU6050 mpu;

// WiFi credentials
const char *ssid = "TESTESP";
const char *password = "12345678";

// Backend server for AI queries
const char *aiServer = "http://192.168.239.118:3000/ask-ai";

// NTP settings
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

uint32_t tsLastReport = 0;
bool beatDetected = false;
unsigned long lastStepTime = 0;
const unsigned long stepDebounceTime = 300;
int stepCount = 0;

// Store AI response
String lastAIResponse = "";

void onBeatDetected()
{
  Serial.println("Beat Detected!");
  beatDetected = true;
}

const unsigned char heartBitmap[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x18, 0x00, 0x0f, 0xe0, 0x7f, 0x00, 0x3f, 0xf9, 0xff, 0xc0,
    0x7f, 0xf9, 0xff, 0xc0, 0x7f, 0xff, 0xff, 0xe0, 0x7f, 0xff, 0xff, 0xe0, 0xff, 0xff, 0xff, 0xf0,
    0xff, 0xf7, 0xff, 0xf0, 0xff, 0xe7, 0xff, 0xf0, 0xff, 0xe7, 0xff, 0xf0, 0x7f, 0xdb, 0xff, 0xe0,
    0x7f, 0x9b, 0xff, 0xe0, 0x00, 0x3b, 0xc0, 0x00, 0x3f, 0xf9, 0x9f, 0xc0, 0x3f, 0xfd, 0xbf, 0xc0,
    0x1f, 0xfd, 0xbf, 0x80, 0x0f, 0xfd, 0x7f, 0x00, 0x07, 0xfe, 0x7e, 0x00, 0x03, 0xfe, 0xfc, 0x00,
    0x01, 0xff, 0xf8, 0x00, 0x00, 0xff, 0xf0, 0x00, 0x00, 0x7f, 0xe0, 0x00, 0x00, 0x3f, 0xc0, 0x00,
    0x00, 0x0f, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void setup()
{
  Serial.begin(115200);
  Wire.begin(D2, D1);  //SCL D1, SDA D2

  oled.begin();
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(1);
  oled.setCursor(0, 0);
  oled.println("Connecting WiFi...");
  oled.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  if (!pox.begin())
  {
    oled.setCursor(0, 10);
    oled.println("MAX30100 Fail");
    oled.display();
    while (1)
      ;
  }
  pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
  pox.setOnBeatDetectedCallback(onBeatDetected);

  if (!bmp.begin(0x76))
  {
    oled.setCursor(0, 20);
    oled.println("BMP280 Fail");
    oled.display();
    while (1)
      ;
  }

  // 🔧 Initialize MPU6050
  mpu.initialize();
  if (!mpu.testConnection())
  {
    oled.setCursor(0, 30);
    oled.println("MPU6050 Fail");
    oled.display();
    while (1);
  }

  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println("All Sensors OK");
  oled.display();
  delay(1500);
}
void detectStep()
{
  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float ax_g = ax / 16384.0;
  float ay_g = ay / 16384.0;
  float az_g = az / 16384.0;

  float accMagnitude = sqrt(ax_g * ax_g + ay_g * ay_g + az_g * az_g);

  static float prevMagnitude = 1.0;
  static bool stepPossible = false;

  if ((accMagnitude - prevMagnitude) > 0.25)
  {
    stepPossible = true;
  }

  if (stepPossible && (prevMagnitude - accMagnitude) > 0.25)
  {
    if (millis() - lastStepTime > stepDebounceTime)
    {
      stepCount++;
      lastStepTime = millis();
    }
    stepPossible = false;
  }

  prevMagnitude = accMagnitude;
}

String getFormattedTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "Time N/A";
  char buffer[16];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
  return String(buffer);
}

String getFormattedDate()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "Date N/A";
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%d %b %Y", &timeinfo);
  return String(buffer);
}

void sendSensorDataToAI(float hr, float spo2, float temp, int steps, String timeStr)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    WiFiClient client;
    http.begin(client, "http://192.168.239.118:3000/data");
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["heartRate"] = hr;
    doc["spo2"] = spo2;
    doc["temperature"] = temp;
    doc["steps"] = steps;
    doc["time"] = timeStr;

    String requestBody;
    serializeJson(doc, requestBody);

    int code = http.POST(requestBody); // 🔧 check response
    Serial.print("Data POST status: ");
    Serial.println(code);

    http.end();
  }
}

void askAI(String query)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    WiFiClient client;
    http.begin(client, aiServer);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<128> doc;
    doc["query"] = query;
    String jsonPayload;
    serializeJson(doc, jsonPayload);

    int httpCode = http.POST(jsonPayload);

    if (httpCode > 0)
    {
      String response = http.getString();
      Serial.println("AI Response:");
      Serial.println(response);

      StaticJsonDocument<256> respDoc;
      DeserializationError err = deserializeJson(respDoc, response);
      if (!err && respDoc.containsKey("response"))
      {
        lastAIResponse = respDoc["response"].as<String>();
      }
      else
      {
        lastAIResponse = response; // fallback raw
      }
    }
    else
    {
      Serial.print("AI Request Failed. HTTP code: ");
      Serial.println(httpCode);
    }

    http.end();
  }
}

void loop()
{
  pox.update();
  detectStep();

  static unsigned long lastDisplayUpdate = 0;
  unsigned long currentMillis = millis();

  if (currentMillis - lastDisplayUpdate > 1000)
  {
    lastDisplayUpdate = currentMillis;

    float bpm = pox.getHeartRate();
    float spo2 = pox.getSpO2();
    float temp = bmp.readTemperature();
    float pressure = bmp.readPressure() / 100.0F;

    if (bpm == 0.0)
      bpm = 72.0;
    if (spo2 == 0.0)
      spo2 = 98.0;

    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print("HR: ");
    oled.print(bpm, 1);
    oled.print(" bpm");
    oled.setCursor(0, 10);
    oled.print("SpO2: ");
    oled.print(spo2, 1);
    oled.print(" %");
    oled.setCursor(0, 20);
    oled.print("Temp: ");
    oled.print(temp, 1);
    oled.print(" C");
    oled.setCursor(0, 30);
    oled.print("Pres: ");
    oled.print(pressure, 1);
    oled.print(" hPa");
    oled.setCursor(0, 40);
    oled.print("Steps: ");
    oled.print(stepCount);
    oled.setCursor(0, 50);
    oled.print(getFormattedTime());
    oled.print(" ");
    oled.print(getFormattedDate());
    oled.drawBitmap(90, 0, heartBitmap, 28, 28, 1);

    // 🔧 Adjust AI response display
    if (lastAIResponse.length() > 0)
    {
      oled.setCursor(0, 60 - 8); // shift up
      oled.setTextSize(1);
      oled.print("AI: ");
      oled.print(lastAIResponse.substring(0, 18));
    }

    oled.display();

    String currentTime = getFormattedTime();
    sendSensorDataToAI(bpm, spo2, temp, stepCount, currentTime);

    if (Serial.available())
    {
      String userInput = Serial.readStringUntil('\n');
      userInput.trim();
      if (userInput.length() > 0)
      {
        askAI(userInput);
      }
    }
  }
}
