#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "MAX30100_PulseOximeter.h"

// Wifi
const char *ssid = "Pahari";
const char *password = "sanjay246";
const char *serverIP = "http://10.187.12.253:5000";
const char *apiUrl = "http://10.187.12.253:5000/vitals";
const char *fenceUrl = "http://10.187.12.253:5000/fence-status";
#define WIFI_ENABLED 1

// Pins Config
#define SDA_PIN 4      // D2 -> GPIO4
#define SCL_PIN 5      // D1 -> GPIO5
#define ONE_WIRE_BUS 2 // D4 -> GPIO2 (DS18B20 DATA, 4.7kΩ to 3.3V)
#define BUZZER_PIN 14  // D5 -> GPIO14
#define LED_PIN 12     // D6 -> GPIO12
#define MAX_INT_PIN 13 // D7 -> GPIO13

#define REPORTING_PERIOD_MS 1000

// DS18B20
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensors(&oneWire);

// Sensors
Adafruit_MPU6050 mpu;
PulseOximeter pox;

// Availability Flags
bool mpuAvailable = false;
bool maxAvailable = false;
bool tempAvailable = false;

// Sensor Data
struct SensorData
{
  float accelX, accelY, accelZ;
  float motionMagnitude;
  int heartRate;
  int spo2;
  float temperature;
  unsigned long timestamp;
};

SensorData data;

// MAX30100 timing
uint32_t tsLastReport = 0;

// Geofence Alert (polled from server)
bool geofenceOutOfBounds = false;
unsigned long lastFencePoll = 0;
const unsigned long fencePollInterval = 2000; // poll every 2s

// Circular Buffer
#define BUFFER_SIZE 20
SensorData buffer[BUFFER_SIZE];
int bufferIndex = 0;
int bufferCount = 0;

// Web Server
ESP8266WebServer server(80);

// API Timing
unsigned long lastApiSend = 0;

bool enablesim = true;
bool pulsesim = true;
bool temsim = false;
unsigned long durmin = 10000;
unsigned long startsimmillis = 0;

//  Function declarations
void readMPU();
void readMAX30100();
void readTemperature();
void collectSensors();
void handleAlerts();
void pollFence();
void printData();
void sendToAPI();
void setupWebServerRoutes();
void checkSerialCommands();
void cycleBaudTest();

// FallBack
float simHeartRate();
float simSpO2();
float simTemperature();

//  beat detection
void onBeatDetected()
{
  Serial.println("Beat detected!");
}


void setup()
{
  Serial.begin(115200);
  delay(500);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(MAX_INT_PIN, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  // MPU6050
  for (int i = 0; i < 4; i++)
  {
    if (mpu.begin())
    {
      mpuAvailable = true;
      break;
    }
    Serial.println("MPU6050 init failed, retrying...");
    delay(250);
  }
  Serial.println(mpuAvailable ? "MPU6050 ready." : "MPU6050 not found; continuing.");

  // MAX30100
  for (int i = 0; i < 4; i++)
  {
    if (pox.begin())
    {
      maxAvailable = true;
      break;
    }
    Serial.println("MAX30100 init failed, retrying...");
    delay(250);
  }
  if (maxAvailable)
  {
    pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
    pox.setOnBeatDetectedCallback(onBeatDetected);
    Serial.println("MAX30100 ready.");
  }
  else
  {
    Serial.println("MAX30100 not found");
  }

  // start simulation window timer
  startsimmillis = millis();

  // DS18B20
  tempSensors.begin();
  int deviceCount = tempSensors.getDeviceCount();
  if (deviceCount > 0)
  {
    tempAvailable = true;
    Serial.print("DS18B20 found. Devices: ");
    Serial.println(deviceCount);
  }
  else
  {
    Serial.println("DS18B20");
  }

  // WiFi
#if WIFI_ENABLED
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000)
  {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi connected!" : "\nWiFi timeout; continuing offline.");
  setupWebServerRoutes();
#else
  Serial.println("WiFi disabled by config.");
#endif
}

//  web routes setup
void setupWebServerRoutes()
{
#if WIFI_ENABLED
  server.on("/buffer", []()
            {
    String json = "{\"samples\":[";
    for (int i = 0; i < bufferCount; i++) {
      int idx = (bufferIndex - bufferCount + i + BUFFER_SIZE) % BUFFER_SIZE;
      SensorData &s = buffer[idx];
      if (i > 0) json += ",";
      json += "{";
      json += "\"ts\":"          + String(s.timestamp)          + ",";
      json += "\"motion\":"      + String(s.motionMagnitude, 2) + ",";
      json += "\"accelX\":"      + String(s.accelX, 2)          + ",";
      json += "\"accelY\":"      + String(s.accelY, 2)          + ",";
      json += "\"accelZ\":"      + String(s.accelZ, 2)          + ",";
      json += "\"heartRate\":"   + String(s.heartRate)          + ",";
      json += "\"spo2\":"        + String(s.spo2)               + ",";
      json += "\"temperature\":" + String(s.temperature, 1);
      json += "}";
    }
    json += "]}";
    server.send(200, "application/json", json); });
  server.begin();
#endif
}

void loop()
{
  collectSensors();
  pollFence();
  handleAlerts();
  printData();
  sendToAPI();

#if WIFI_ENABLED
  server.handleClient();
#endif

  checkSerialCommands();

  delay(200); // Slow down loop to ~5 Hz
}

//  sensor readers
void readMPU()
{
  if (!mpuAvailable)
  {
    data.accelX = data.accelY = data.accelZ = 0.0;
    data.motionMagnitude = 0.0;
    return;
  }
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  data.accelX = a.acceleration.x;
  data.accelY = a.acceleration.y;
  data.accelZ = a.acceleration.z;
  data.motionMagnitude = sqrt(
      data.accelX * data.accelX +
      data.accelY * data.accelY +
      data.accelZ * data.accelZ);
}

void readMAX30100()
{
  bool useSim = false;
  if (enablesim && pulsesim)
  {
    if (!maxAvailable)
      useSim = true;
    else if (millis() - startsimmillis < durmin)
      useSim = true;
  }

  if (useSim)
  {
    data.heartRate = (int)(simHeartRate() + 0.5f);
    data.spo2 = (int)(simSpO2() + 0.5f);
  }
  else
  {
    if (maxAvailable)
    {
      pox.update();
      float hr = pox.getHeartRate();
      float spo2 = pox.getSpO2();
      bool hrValid = (hr > 30.0f && hr < 220.0f);
      bool spo2Valid = (spo2 > 80.0f && spo2 <= 100.0f);
      if (hrValid)
        data.heartRate = (int)(hr + 0.5f);
      else if (enablesim)
        data.heartRate = (int)(simHeartRate() + 0.5f);
      else
        data.heartRate = -1;
      if (spo2Valid)
        data.spo2 = (int)(spo2 + 0.5f);
      else if (enablesim)
        data.spo2 = (int)(simSpO2() + 0.5f);
      else
        data.spo2 = -1;
    }
    else
    {
      // no sensor and simulation disabled
      data.heartRate = -1;
      data.spo2 = -1;
    }
  }

  if (millis() - tsLastReport > REPORTING_PERIOD_MS)
  {
    Serial.print("MAX -> HR: ");
    Serial.print(data.heartRate);
    Serial.print(" bpm | SpO2: ");
    Serial.print(data.spo2);
    Serial.println(" %");
    tsLastReport = millis();
  }
}

void readTemperature()
{
  bool useSim = false;
  if (enablesim && temsim)
  {
    if (!tempAvailable)
      useSim = true;
    else if (millis() - startsimmillis < durmin)
      useSim = true;
  }

  if (useSim)
  {
    data.temperature = simTemperature();
    return;
  }

  if (tempAvailable)
  {
    tempSensors.requestTemperatures();
    float t = tempSensors.getTempCByIndex(0);
    bool valid = (t > 10.0f && t < 50.0f);
    data.temperature = valid ? t : -1.0f;
  }
  else
  {
    // no sensor and simulation disabled -> mark invalid
    data.temperature = -1.0f;
  }
}

//  collect all sensor
void collectSensors()
{
  readMPU();
  readMAX30100();
  readTemperature();
  data.timestamp = millis();

  buffer[bufferIndex] = data;
  bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
  if (bufferCount < BUFFER_SIZE)
    bufferCount++;
}

void pollFence()
{
#if WIFI_ENABLED
  if (millis() - lastFencePoll < fencePollInterval)
    return;
  lastFencePoll = millis();

  if (WiFi.status() != WL_CONNECTED)
    return;

  WiFiClient client;
  HTTPClient http;
  http.begin(client, fenceUrl);
  http.addHeader("ngrok-skip-browser-warning", "true");
  int code = http.GET();

  if (code == 200)
  {
    String payload = http.getString();
    // Simple parse: look for "out_of_bounds":true
    geofenceOutOfBounds = (payload.indexOf("true") > 0);
    Serial.print("Fence status: ");
    Serial.println(geofenceOutOfBounds ? "OUT OF BOUNDS" : "IN BOUNDS");
  }
  else
  {
    Serial.print("Fence poll failed: ");
    Serial.println(code);
  }
  http.end();
#endif
}

void handleAlerts()
{
  // Buzzer + LED are active-LOW (sound/light when pin is LOW)
  digitalWrite(LED_PIN, geofenceOutOfBounds ? LOW : HIGH);
  digitalWrite(BUZZER_PIN, geofenceOutOfBounds ? LOW : HIGH);
}

void printData()
{
  Serial.print("Motion: ");
  Serial.print(data.motionMagnitude, 2);
  Serial.print(" | Accel: ");
  Serial.print(data.accelX, 2);
  Serial.print(", ");
  Serial.print(data.accelY, 2);
  Serial.print(", ");
  Serial.print(data.accelZ, 2);
  Serial.print(" | HR: ");
  Serial.print(data.heartRate);
  Serial.print(" | SpO2: ");
  Serial.print(data.spo2);
  Serial.print(" | Temp: ");
  Serial.print(data.temperature, 1);
  Serial.print(" C | ts: ");
  Serial.println(data.timestamp);
}

void sendToAPI()
{
  if (millis() - lastApiSend < 5000)
    return;
  lastApiSend = millis();

#if WIFI_ENABLED
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected - skipping API send.");
    return;
  }

  // Send latest vitals snapshot
  String json = "{";
  json += "\"heartRate\":" + String(data.heartRate) + ",";
  json += "\"spo2\":" + String(data.spo2) + ",";
  json += "\"temperature\":" + String(data.temperature, 1) + ",";
  json += "\"motion\":" + String(data.motionMagnitude, 2);
  json += "}";

  WiFiClient client;
  HTTPClient http;
  http.begin(client, apiUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("ngrok-skip-browser-warning", "true");
  int code = http.POST(json);
  Serial.print("Vitals API Response: ");
  Serial.println(code);
  http.end();
#else
  Serial.println("WiFi disabled; skipping API send.");
#endif
}

void checkSerialCommands()
{
  if (Serial.available())
  {
    char c = Serial.read();
    if (c == 'b' || c == 'B')
      cycleBaudTest();
  }
}

void cycleBaudTest()
{
  const long rates[] = {9600, 19200, 38400, 57600, 74880, 115200};
  const int n = sizeof(rates) / sizeof(rates[0]);
  Serial.println("Cycling baud rates 3s each...");
  for (int i = 0; i < n; i++)
  {
    Serial.end();
    delay(50);
    Serial.begin(rates[i]);
    Serial.print("BAUD_TEST:");
    Serial.println(rates[i]);
    delay(3000);
  }
  Serial.end();
  delay(50);
  Serial.begin(115200);
  Serial.println("Restored baud to 115200");
}

float simHeartRate()
{
  static float base = 80;
  static float drift = 0.0;
  static unsigned long lastCall = 0;

  unsigned long now = millis();
  float dt = (now - lastCall) / 1000.0;
  lastCall = now;

  drift += ((float)random(-30, 31) / 10.0) * dt;
  drift = constrain(drift, -12.0, 12.0);

  float noise = (float)random(-20, 21) / 5.0;
  if (random(0, 100) < 3)
  {
    noise += (float)random(-30, 31) / 2.0;
  }

  float hr = base + drift + noise;
  return constrain(hr, 50.0, 130.0);
}

float simSpO2()
{
  static float base = 97.5;
  static float drift = 0.0;
  static unsigned long lastCall = 0;

  unsigned long now = millis();
  float dt = (now - lastCall) / 1000.0;
  lastCall = now;

  drift += ((float)random(-15, 16) / 10.0) * dt;
  drift = constrain(drift, -3.0, 3.0);

  float noise = (float)random(-8, 9) / 10.0;
  if (random(0, 200) < 5)
  {
    noise += (float)random(-30, 31) / 10.0;
  }

  float spo2 = base + drift + noise;
  return constrain(spo2, 85.0, 100.0);
}

float simTemperature()
{
  static float base = 28.3;
  static float drift = 0.0;
  static unsigned long lastCall = 0;

  unsigned long now = millis();
  float dt = (now - lastCall) / 1000.0;
  lastCall = now;

  drift += ((float)random(-8, 9) / 50.0) * dt;
  drift = constrain(drift, -0.8, 0.8);

  float noise = (float)random(-20, 21) / 100.0;
  if (random(0, 200) < 4)
  {
    noise += (float)random(-50, 51) / 100.0;
  }

  float temp = base + drift + noise;
  return constrain(temp, 34.5, 39.0);
}