/*
  ESP32 ANCHOR DEVICE (x3)
  ========================
  Each anchor scans for the target ESP32 by MAC address,
  measures RSSI, and sends it to your PC server via HTTP POST.

  SETUP FOR EACH ANCHOR:
  1. Change ANCHOR_ID to "anchor1", "anchor2", or "anchor3"
  2. Change ANCHOR_LAT and ANCHOR_LON to real GPS coordinates
  3. Set TARGET_MAC to the MAC printed by the target ESP32
  4. Set WiFi credentials
  5. Set SERVER_IP to your PC's local IP address
*/

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─── CONFIGURE THESE FOR EACH ANCHOR ─────────────────────────────────────────
const char* ANCHOR_ID   = "anchor3";       // Change: anchor1 / anchor2 / anchor3
const double ANCHOR_LAT = 27.6195309;       // Set real lat of this anchor
const double ANCHOR_LON = 85.5371227;       // Set real lon of this anchor
// ─────────────────────────────────────────────────────────────────────────────

// ─── SHARED CONFIG (same for all anchors) ─────────────────────────────────────
const char* WIFI_SSID     = "Pahari";
const char* WIFI_PASSWORD = "sanjay246";


// MAC address of the target ESP32 (printed on serial when target boots)
// Format: "AA:BB:CC:DD:EE:FF" — all uppercase with colons
const char* TARGET_MAC = "4C:75:25:37:D6:74";

// Your PC's local IP (run `ipconfig` on Windows or `ifconfig` on Mac/Linux)
const char* SERVER_IP = "10.187.12.253";
const int   SERVER_PORT = 5000;
// ─────────────────────────────────────────────────────────────────────────────

// RSSI smoothing — Kalman-like simple exponential moving average
float smoothedRSSI = -80.0;
const float ALPHA  = 0.15;  // Lower = smoother, calibrated for noisy environment

// Path loss model parameters — calibrate these for your environment
// RSSI_REF: measured RSSI at exactly 1 meter from target
// PATH_LOSS_EXP: 2.0 = open space, 2.5–3.5 = indoor with walls
const float RSSI_REF      = -60.8;  // Measured avg RSSI at 1m
const float PATH_LOSS_EXP = 2.0;    // Calibrated (min practical value)

// Internal state
String targetMacUpper = "";
int    rssiBuffer[10];
int    bufferIndex    = 0;
bool   bufferFull     = false;

// ── Promiscuous mode callback — fires on every WiFi packet nearby ─────────────
void IRAM_ATTR snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;  // Mgmt + data frames

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

    // Store in circular buffer for averaging
    rssiBuffer[bufferIndex] = rawRSSI;
    bufferIndex = (bufferIndex + 1) % 10;
    if (bufferIndex == 0) bufferFull = true;

    // Exponential moving average
    smoothedRSSI = ALPHA * rawRSSI + (1.0 - ALPHA) * smoothedRSSI;
  }
}

// ── RSSI → Distance conversion ────────────────────────────────────────────────
float rssiToDistance(float rssi) {
  // d = 10 ^ ((RSSI_REF - RSSI) / (10 * n))
  return pow(10.0, (RSSI_REF - rssi) / (10.0 * PATH_LOSS_EXP));
}

// ── Average from buffer ───────────────────────────────────────────────────────
float getBufferAverage() {
  int count = bufferFull ? 10 : bufferIndex;
  if (count == 0) return smoothedRSSI;
  float sum = 0;
  for (int i = 0; i < count; i++) sum += rssiBuffer[i];
  return sum / count;
}

// ── Send data to PC server ────────────────────────────────────────────────────
void sendToServer(float rssi, float distance) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/rssi";
  http.begin(url);
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
    Serial.printf("[%s] Sent → RSSI: %.1f dBm | Distance: %.2f m | HTTP: %d\n",
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

  // Uppercase target MAC for comparison
  targetMacUpper = String(TARGET_MAC);
  targetMacUpper.toUpperCase();

  // Connect to WiFi first
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[ANCHOR] Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[ANCHOR] WiFi connected: " + WiFi.localIP().toString());

  // Enable promiscuous mode to sniff packets
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE); // Match your router's channel

  Serial.printf("[%s] Listening for MAC: %s\n", ANCHOR_ID, TARGET_MAC);
}

unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 1000; // Send every 1 second

void loop() {
  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();

    float avgRSSI    = getBufferAverage();
    float distance   = rssiToDistance(avgRSSI);

    sendToServer(avgRSSI, distance);
  }
}
