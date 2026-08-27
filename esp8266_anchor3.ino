/*
  ESP8266 ANCHOR DEVICE (anchor3)
  ===============================
  Scans for the target ESP device by MAC address,
  measures RSSI, and sends it to your PC server via HTTP POST.

  SETUP:
  1. Confirm ANCHOR_ID is "anchor3"
  2. Set ANCHOR_LAT and ANCHOR_LON to real GPS coordinates
  3. Set TARGET_MAC to the MAC printed by the target ESP32
  4. Set WiFi credentials
  5. Set SERVER_IP to your PC's local IP address
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

extern "C" {
#include "user_interface.h"
}

#ifndef WIFI_PROMISCUOUS_PKT_T
// Fallback for ESP8266 cores that do not expose wifi_promiscuous_pkt_t
typedef struct {
  signed rssi : 8;
  unsigned : 24;
} wifi_pkt_rx_ctrl_t;

typedef struct {
  wifi_pkt_rx_ctrl_t rx_ctrl;
  uint8_t payload[0];
} wifi_promiscuous_pkt_t;
#endif

// ─── CONFIGURE FOR THIS ANCHOR ───────────────────────────────────────────────
const char* ANCHOR_ID   = "anchor3";
const double ANCHOR_LAT = 27.700769;
const double ANCHOR_LON = 85.314940;
// ─────────────────────────────────────────────────────────────────────────────

// ─── SHARED CONFIG (same for all anchors) ─────────────────────────────────────
const char* WIFI_SSID     = "Pahari";
const char* WIFI_PASSWORD = "sanjay246";

// MAC address of the target ESP32 (printed on serial when target boots)
// Format: "AA:BB:CC:DD:EE:FF" — all uppercase with colons
const char* TARGET_MAC = "4C:75:25:37:D6:74";

// Your PC's local IP (run `ipconfig` on Windows or `ifconfig` on Mac/Linux)
const char* SERVER_IP   = "10.187.12.253";
const int   SERVER_PORT = 5000;
// ─────────────────────────────────────────────────────────────────────────────

// RSSI smoothing — exponential moving average
float smoothedRSSI = -80.0;
const float ALPHA  = 0.15;

// Path loss model parameters — calibrate these for your environment
const float RSSI_REF      = -60.8;  // Measured avg RSSI at 1m
const float PATH_LOSS_EXP = 2.0;    // Calibrated (min practical value)

// Internal state
String targetMacUpper = "";
int    rssiBuffer[10];
int    bufferIndex    = 0;
bool   bufferFull     = false;

// ── Promiscuous mode callback (ESP8266) ──────────────────────────────────────
void ICACHE_RAM_ATTR snifferCallback(uint8_t* buf, uint16_t len) {
  if (len < 24) return;  // Minimum 802.11 header length

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;

  // Source MAC address is at bytes 10-15 in the 802.11 header
  const uint8_t* mac = pkt->payload + 10;
  char macStr[18];
  snprintf(macStr, sizeof(macStr),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );

  if (String(macStr) == targetMacUpper) {
    int rawRSSI = pkt->rx_ctrl.rssi;

    rssiBuffer[bufferIndex] = rawRSSI;
    bufferIndex = (bufferIndex + 1) % 10;
    if (bufferIndex == 0) bufferFull = true;

    smoothedRSSI = ALPHA * rawRSSI + (1.0 - ALPHA) * smoothedRSSI;
  }
}

float rssiToDistance(float rssi) {
  return pow(10.0, (RSSI_REF - rssi) / (10.0 * PATH_LOSS_EXP));
}

float getBufferAverage() {
  int count = bufferFull ? 10 : bufferIndex;
  if (count == 0) return smoothedRSSI;
  float sum = 0;
  for (int i = 0; i < count; i++) sum += rssiBuffer[i];
  return sum / count;
}

void sendToServer(float rssi, float distance) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/rssi";

  if (!http.begin(client, url)) return;
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["anchor_id"]  = ANCHOR_ID;
  doc["anchor_lat"] = ANCHOR_LAT;
  doc["anchor_lon"] = ANCHOR_LON;
  doc["rssi"]       = rssi;
  doc["distance"]   = distance;
  doc["timestamp"]  = millis();

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.printf("[%s] Sent -> RSSI: %.1f dBm | Distance: %.2f m | HTTP: %d\n",
                  ANCHOR_ID, rssi, distance, httpCode);
  } else {
    Serial.printf("[%s] POST failed: %s\n", ANCHOR_ID, http.errorToString(httpCode).c_str());
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\n[%s] Booting anchor...\n", ANCHOR_ID);

  targetMacUpper = String(TARGET_MAC);
  targetMacUpper.toUpperCase();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[ANCHOR] Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[ANCHOR] WiFi connected: " + WiFi.localIP().toString());

  // Enable promiscuous mode to sniff packets
  wifi_set_promiscuous_rx_cb(snifferCallback);
  wifi_promiscuous_enable(1);
  wifi_set_channel(6);  // Match your router's channel

  Serial.printf("[%s] Listening for MAC: %s\n", ANCHOR_ID, TARGET_MAC);
}

unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 1000;

void loop() {
  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();

    float avgRSSI  = getBufferAverage();
    float distance = rssiToDistance(avgRSSI);

    sendToServer(avgRSSI, distance);
  }
}
