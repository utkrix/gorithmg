/*
  ESP32 TARGET DEVICE
  ===================
  Connects to WiFi and sends periodic beacon pings.
  The 3 anchor ESPs will detect this device by its MAC address
  and measure RSSI.

  No changes needed except WiFi credentials.
*/
#include <WiFi.h>
#include <WiFiUdp.h>

const char* WIFI_SSID     = "Pahari";
const char* WIFI_PASSWORD = "sanjay246";

// PC server IP so this device generates traffic the anchors can sniff
const char* SERVER_IP   = "10.187.12.253";
const int   SERVER_PORT = 5001;


// This device's name — anchors will look for this
const char* DEVICE_NAME = "ESP32_TARGET";

WiFiUDP udp;
unsigned long lastPing = 0;
const unsigned long PING_INTERVAL = 1000;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("[TARGET] Booting...");

  // Connect to WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[TARGET] Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n[TARGET] Connected!");
  Serial.print("[TARGET] IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("[TARGET] MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.println("[TARGET] Copy this MAC address into anchor code!");
}

void loop() {
  // Just stay connected — anchors detect us passively via RSSI
  // This loop sends a heartbeat so you can monitor via Serial
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[TARGET] Alive - RSSI from router: " + String(WiFi.RSSI()) + " dBm");

    if (millis() - lastPing >= PING_INTERVAL) {
      lastPing = millis();
      udp.beginPacket(SERVER_IP, SERVER_PORT);
      udp.write("ping");
      udp.endPacket();
    }
  } else {
    Serial.println("[TARGET] WiFi lost, reconnecting...");
    WiFi.reconnect();
  }
  delay(2000);
}
