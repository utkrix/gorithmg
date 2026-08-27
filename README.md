# Gorithm

WiFi RSSI trilateration for tracking a moving target without GPS on the tag itself. Three fixed anchors listen for radio traffic from a small ESP device, estimate distance from signal strength, and a Python server solves for position. A browser dashboard shows the result in real time. The same stack extends to livestock monitoring: a collar reports vitals, a map draws a geofence, and the collar buzzes when the animal leaves the boundary.

This is a working prototype, not a production positioning product. Accuracy depends on calibration and environment. It is useful where you want cheap, hackable location tracking and can tolerate a few meters of error.

---

## What it does

The system has four moving parts:

1. **Target** — An ESP32 or ESP8266 on WiFi. It does not need a GPS module. It just connects to the network and sends periodic packets so anchors have something to hear.
2. **Anchors (×3)** — ESP32 or ESP8266 boards in WiFi promiscuous mode. Each one filters packets by the target's MAC address, reads RSSI, smooths the readings, converts signal strength to an estimated distance, and POSTs the result to the server along with its own fixed GPS coordinates.
3. **Server** — A Flask app that collects anchor reports, runs least-squares trilateration when at least three anchors are live, and pushes state to clients over Server-Sent Events.
4. **Dashboard** — Either a lightweight HTML view (`index.html`) or a Mapbox-based map (`mapbox/`) with anchor circles, live target position, optional phone GPS overlay, geofence drawing, and vitals readout.

An optional **animal collar** (`animal.ino`) adds MAX30100 pulse oximetry, MPU6050 motion, DS18B20 temperature, and a buzzer/LED that reacts when the server reports an out-of-bounds geofence status.

A companion **GPS tracker app** (`gps-tracker-app/`) is a small Expo/React Native client that streams phone GPS to the server. That is useful for comparing RSSI-derived position against a ground-truth fix, or for tracking a handler alongside the target.

---

## How positioning works

RSSI (Received Signal Strength Indicator) drops as distance increases. Each anchor applies a log-distance path loss model:

```
distance = 10 ^ ((RSSI_REF - rssi) / (10 × PATH_LOSS_EXP))
```

`RSSI_REF` is the measured signal at 1 meter. `PATH_LOSS_EXP` (often called *n*) describes how quickly signal fades. Open air is around 2.0; indoor spaces with walls and furniture are typically 2.5–3.5.

With three or more distance estimates from known anchor positions, the server converts lat/lon to local meters and minimizes the sum of squared range errors (Nelder-Mead via SciPy). The result includes an RMS residual in meters, which is a rough confidence indicator rather than a guaranteed error bound.

Anchors also apply an exponential moving average and a short circular buffer before reporting, which cuts down on single-packet spikes.

---

## Architecture

```
                    ┌─────────────┐
                    │   Target    │  WiFi traffic (no GPS)
                    │ ESP32/8266  │
                    └──────┬──────┘
                           │ 802.11 frames
         ┌─────────────────┼─────────────────┐
         ▼                 ▼                 ▼
   ┌───────────┐     ┌───────────┐     ┌───────────┐
   │  Anchor 1 │     │  Anchor 2 │     │  Anchor 3 │
   │  (fixed   │     │  (fixed   │     │  (fixed   │
   │   GPS)    │     │   GPS)    │     │   GPS)    │
   └─────┬─────┘     └─────┬─────┘     └─────┬─────┘
         │    RSSI + distance + anchor lat/lon
         └─────────────────┼─────────────────┘
                           ▼
                    ┌─────────────┐
                    │  server.py  │  Trilateration + SSE
                    │  Flask :5000│
                    └──────┬──────┘
                           │
         ┌─────────────────┼─────────────────┐
         ▼                 ▼                 ▼
   index.html         mapbox/           animal.ino
   (simple map)    (geofence + vitals)  (collar alerts)
                           ▲
                    gps-tracker-app
                    (phone GPS reference)
```

---

## Use cases

### Livestock and pasture monitoring

The original motivation. Put anchors at known points around a field or barn. The animal wears the target chip (or a collar that also streams vitals). Draw a polygon geofence on the Mapbox dashboard. When trilateration places the target outside the fence, the server flag updates and the collar can sound a buzzer. Heart rate, SpO2, temperature, and motion status appear on the map overlay.

### Indoor and semi-outdoor asset tracking

GPS fails or drifts inside warehouses, stables, greenhouses, and covered yards. If you can mount three WiFi anchors with known positions and calibrate path loss for that space, RSSI trilateration gives you room-scale tracking without UWB hardware or a commercial RTLS install. Expect several meters of jitter; good enough for "which bay is the pallet in?" rather than centimeter robotics.

### Campus and yard experiments

Anchors on building corners or fence posts, target on a cart, drone, or person. Useful for coursework, algorithm comparison, or prototyping before committing to BLE beacons or ultra-wideband.

### Phone vs. RSSI comparison

Run the GPS app on a phone carried next to the target device. The server stores both fixes. You can visually compare trilateration output to GPS on the map and tune `RSSI_REF` and `PATH_LOSS_EXP` per anchor.

### What it is not good for

- Sub-meter precision navigation
- Fast-moving targets in multipath-heavy environments without retuning
- Security-sensitive access control (RSSI is spoofable)
- Large multi-floor buildings without per-floor anchor sets and separate calibration

---

## Repository layout

| Path | Role |
|------|------|
| `server.py` | Trilateration engine, REST API, SSE stream, static dashboard host |
| `index.html` | Minimal live dashboard (anchors, target, schematic map) |
| `mapbox/` | Mapbox GL map with geofence editor, vitals panel, anchor coverage circles |
| `esp32_anchor1.ino`, `esp32_anchor2.ino`, `esp32_anchor3.ino` | ESP32 anchor firmware |
| `esp8266_anchor3.ino` | ESP8266 variant of anchor firmware |
| `esp32_target.ino`, `esp8266_target.ino` | Target device firmware |
| `animal.ino` | Collar: sensors, vitals upload, geofence poll, buzzer/LED |
| `gps-tracker-app/` | Expo app posting phone GPS to `/gps` |

---

## Requirements

**Server (PC or Raspberry Pi on the same LAN as the ESPs)**

- Python 3.8+
- `pip install flask flask-cors scipy numpy`

**Hardware (typical build)**

- 3× ESP32 or ESP8266 (anchors)
- 1× ESP32 or ESP8266 (target)
- Optional: ESP8266 + MAX30100 + MPU6050 + DS18B20 (collar)
- WiFi access point all devices share
- Anchor positions surveyed in GPS (lat/lon entered in each anchor sketch)

**Mapbox dashboard**

- Node.js 18+
- Mapbox access token in `mapbox/.env` as `VITE_MAPBOX_ACCESS_TOKEN`

**GPS app**

- Node.js, Expo CLI
- Android or iOS device with location permission

---

## Setup

### 1. Network and server

Find your PC's LAN IP (`ip addr`, `ifconfig`, or `ipconfig`). All firmware files need matching `WIFI_SSID`, `WIFI_PASSWORD`, and `SERVER_IP`.

```bash
pip install flask flask-cors scipy numpy
python server.py
```

The server listens on `http://0.0.0.0:5000`. Open `http://<your-ip>:5000/` for the built-in dashboard, or `http://<your-ip>:5000/health` to confirm it is running.

### 2. Flash the target

Upload `esp32_target.ino` or `esp8266_target.ino`. Open the serial monitor at 115200 baud and copy the printed **MAC address**.

### 3. Flash the anchors

For each of the three anchors:

1. Set `ANCHOR_ID` to `anchor1`, `anchor2`, or `anchor3`.
2. Set `ANCHOR_LAT` and `ANCHOR_LON` to the physical install location.
3. Set `TARGET_MAC` to the target's MAC (uppercase, colon-separated).
4. Set `SERVER_IP` to your PC's IP.

ESP32 and ESP8266 anchor code is separate; pick the file that matches your board. Path loss constants (`RSSI_REF`, `PATH_LOSS_EXP`) are per-anchor in the repo because each install site differs—plan to remeasure.

**Calibration tip:** Stand at exactly 1 m from the target with a clear path. Note the average RSSI and set `RSSI_REF` to that value. Walk known distances and adjust `PATH_LOSS_EXP` until reported ranges match reality.

### 4. Verify tracking

With all three anchors online and the target powered, the server log should print lines like:

```
[SERVER] Target -> lat:27.619431 lon:85.537037 +/-2.1m
```

The dashboard shows anchor RSSI, estimated distances, and the computed target position.

### 5. Mapbox dashboard (optional)

```bash
cd mapbox
npm install
# create .env with VITE_MAPBOX_ACCESS_TOKEN=pk...
npm run dev
```

Point `SERVER_URL` in `mapbox/src/main.js` at your Flask server if not using the Vite proxy. Click the map to draw a fence polygon; close it to enable geofence checks. Target and phone markers are checked with Turf.js; out-of-bounds state is POSTed to `/fence-status`.

### 6. Animal collar (optional)

Flash `animal.ino` with the same WiFi and server settings. Wire sensors per the pin defines at the top of the file. The collar POSTs vitals to `/vitals` every second and polls `/fence-status` every two seconds. When `out_of_bounds` is true, the buzzer and LED activate.

### 7. GPS reference app (optional)

```bash
cd gps-tracker-app
npm install
npx expo start
```

Set `SERVER_URL` in `App.js` to your Flask host. Start tracking; coordinates POST to `/gps` once per second.

---

## API reference

| Method | Path | Description |
|--------|------|-------------|
| POST | `/rssi` | Anchors submit `{ anchor_id, anchor_lat, anchor_lon, rssi, distance }` |
| POST | `/gps` | Phone submits `{ lat, lon }` |
| POST | `/vitals` | Collar submits `{ heartRate, spo2, temperature, motion }` |
| GET/POST | `/fence-status` | GET: collar polls `{ out_of_bounds }`. POST: dashboard sets it |
| GET | `/state` | Snapshot of anchors, target, phone, vitals, geofence |
| GET | `/stream` | SSE stream of the same state object |
| GET | `/health` | Server uptime and active anchor count |

Stale data is dropped automatically: anchors after ~5 s, phone after ~5 s, vitals after ~10 s.

---

## Accuracy and limitations

RSSI trilateration is sensitive to:

- **Multipath** — Reflections off metal, wet ground, and walls bias readings.
- **Body absorption** — A target worn on a person or animal attenuates signal differently depending on orientation.
- **WiFi channel congestion** — More traffic can help anchors see packets, but noise increases variance.
- **Anchor geometry** — Collinear anchors produce weak fixes. Spread them in a triangle around the tracked area.

The server reports RMS residual as `accuracy_m`. Treat it as a relative quality score. In field tests, 2–5 m is typical after calibration; uncalibrated installs can be much worse.

For indoor use, raise `PATH_LOSS_EXP`, remeasure `RSSI_REF` inside the space, and expect to recalibrate if furniture or stock layout changes.

---

## License

No license file is included. Add one before distributing or reusing commercially.
