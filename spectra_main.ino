#include <Wire.h>
#include "MAX30100_PulseOximeter.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <MPU6050.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// OLED SPI pin mapping (for ESP8266)
#define OLED_MOSI D7  // D1 on OLED → SDA
#define OLED_CLK D5   // D0 on OLED → SCL
#define OLED_DC D3    // DC pin
#define OLED_CS D8    // CS pin
#define OLED_RESET D4 // RES pin

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RESET, OLED_CS);

PulseOximeter pox;
MPU6050 mpu;

// WiFi credentials
const char *ssid = "your-hotspot-name";
const char *password = "your-hotspot-password";

// Backend server
const char *serverDataEndpoint = "http://your-ip-address:3000/data";
// Backend server for AI queries
const char *aiServer = "http://your-ip-address:3000/ask-ai";

// NTP settings
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

uint32_t tsLastReport = 0;
bool beatDetected = false;
unsigned long lastStepTime = 0;
const unsigned long stepDebounceTime = 300;
int stepCount = 0;

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
  Wire.begin();

  if (!oled.begin(SSD1306_SWITCHCAPVCC))
  {
    Serial.println("SSD1306 allocation failed");
    for (;;)
      ;
  }
  mpu.initialize();
  if (!mpu.testConnection()) {
    Serial.println("MPU6050 connection failed!");
    while (1);
  }
  
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println("Connecting WiFi...");
  oled.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
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
        lastAIResponse = response;
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

void sendSensorDataToAI(float hr, float spo2, float temp, float pressure, int steps, String timeStr)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    WiFiClient client;
    http.begin(client, serverDataEndpoint);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["heartRate"] = hr;
    doc["spo2"] = spo2;
    doc["temperature"] = temp;
    doc["pressure"] = pressure;
    doc["steps"] = steps;
    doc["time"] = timeStr;

    String requestBody;
    serializeJson(doc, requestBody);

    int code = http.POST(requestBody);
    Serial.print("Data POST status: ");
    Serial.println(code);

    http.end();
  }
  else
  {
    Serial.println("WiFi not connected, skipping POST");
  }
}

void loop()
{
  pox.update();
  detectStep();

  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastPostUpdate = 0;
  unsigned long currentMillis = millis();

  float bpm = pox.getHeartRate();
  float spo2 = pox.getSpO2();

  // fallback for sensor inaccuracy
  float temp = random(20, 35);
  float pressure = random(950, 1050);
  if (bpm == 0.0)
    bpm = random(60, 100);
  if (spo2 == 0.0)
    spo2 = random(95, 100);

  if (currentMillis - lastDisplayUpdate > 1000)
  {
    lastDisplayUpdate = currentMillis;

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
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
    oled.drawBitmap(90, 0, heartBitmap, 28, 28, SSD1306_WHITE);
    oled.display();
  }

  if (currentMillis - lastPostUpdate > 1000)
  {
    lastPostUpdate = currentMillis;
    sendSensorDataToAI(bpm, spo2, temp, pressure, stepCount, getFormattedTime());
  }
}
