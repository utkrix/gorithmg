"""
PC SERVER - Trilateration Engine
=================================
Receives RSSI from 3 ESP32 anchors, calculates target position
using trilateration, and serves it to the browser map via SSE.

INSTALL DEPENDENCIES:
  pip install flask flask-cors scipy numpy

RUN:
  python server.py

Then open index.html in your browser.
"""

from flask import Flask, request, jsonify, Response, send_from_directory
from flask_cors import CORS
import numpy as np
from scipy.optimize import minimize
import json
import time
import threading
import math

app = Flask(__name__)
CORS(app)

# ── State ─────────────────────────────────────────────────────────────────────
anchors = {}          # { anchor_id: { lat, lon, rssi, distance, timestamp } }
target_position = None
phone_position = None   # { lat, lon, timestamp }
geofence_status = {'out_of_bounds': False, 'timestamp': 0}
animal_vitals = None    # { heartRate, spo2, temperature, motion, timestamp }
lock = threading.Lock()

# ── Trilateration ─────────────────────────────────────────────────────────────

def haversine_distance_m(lat1, lon1, lat2, lon2):
    """Great-circle distance in meters between two GPS points."""
    R = 6371000  # Earth radius in meters
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(phi1)*math.cos(phi2)*math.sin(dlambda/2)**2
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))

def latlon_to_meters(lat, lon, ref_lat, ref_lon):
    """Convert lat/lon to local XY meters relative to a reference point."""
    x = haversine_distance_m(ref_lat, ref_lon, ref_lat, lon)
    if lon < ref_lon:
        x = -x
    y = haversine_distance_m(ref_lat, ref_lon, lat, ref_lon)
    if lat < ref_lat:
        y = -y
    return x, y

def meters_to_latlon(x, y, ref_lat, ref_lon):
    """Convert local XY meters back to lat/lon."""
    lat = ref_lat + (y / 6371000) * (180 / math.pi)
    lon = ref_lon + (x / (6371000 * math.cos(math.radians(ref_lat)))) * (180 / math.pi)
    return lat, lon

def trilaterate(anchor_data):
    """
    Given 3+ anchors with lat/lon and distance estimates,
    find the most likely target position using least-squares optimization.
    """
    if len(anchor_data) < 3:
        return None

    # Use first anchor as reference origin
    ref = anchor_data[0]
    ref_lat, ref_lon = ref['lat'], ref['lon']

    # Convert all anchors to local XY
    points = []
    distances = []
    for a in anchor_data:
        x, y = latlon_to_meters(a['lat'], a['lon'], ref_lat, ref_lon)
        points.append((x, y))
        distances.append(a['distance'])

    # Cost function: sum of squared errors between estimated and actual distances
    def cost(pos):
        total = 0
        for (ax, ay), d in zip(points, distances):
            estimated = math.sqrt((pos[0]-ax)**2 + (pos[1]-ay)**2)
            total += (estimated - d)**2
        return total

    # Initial guess: centroid of anchor positions
    x0 = np.mean([p[0] for p in points])
    y0 = np.mean([p[1] for p in points])

    result = minimize(cost, [x0, y0], method='Nelder-Mead',
                      options={'xatol': 0.01, 'fatol': 0.01, 'maxiter': 1000})

    if result.success or result.fun < 5.0:  # Accept if error < 5m²
        est_lat, est_lon = meters_to_latlon(result.x[0], result.x[1], ref_lat, ref_lon)
        residual = math.sqrt(result.fun / len(anchor_data))  # RMS error in meters
        return {
            'lat': round(est_lat, 7),
            'lon': round(est_lon, 7),
            'accuracy_m': round(residual, 2),
            'anchors_used': len(anchor_data)
        }
    return None

# ── Routes ────────────────────────────────────────────────────────────────────

@app.route('/rssi', methods=['POST'])
def receive_rssi():
    """Anchors POST their RSSI readings here."""
    global target_position
    data = request.get_json()

    if not data:
        return jsonify({'error': 'No data'}), 400

    required = ['anchor_id', 'anchor_lat', 'anchor_lon', 'rssi', 'distance']
    if not all(k in data for k in required):
        return jsonify({'error': 'Missing fields'}), 400

    anchor_id = data['anchor_id']

    with lock:
        anchors[anchor_id] = {
            'lat':       data['anchor_lat'],
            'lon':       data['anchor_lon'],
            'rssi':      data['rssi'],
            'distance':  data['distance'],
            'timestamp': time.time()
        }

        # Remove stale anchors (no update in 5 seconds)
        now = time.time()
        stale = [k for k, v in anchors.items() if now - v['timestamp'] > 5.0]
        for k in stale:
            del anchors[k]
            print(f"[SERVER] Anchor {k} went stale, removed")

        # Run trilateration if we have all 3
        active = list(anchors.values())
        if len(active) >= 3:
            anchor_list = [{'lat': a['lat'], 'lon': a['lon'], 'distance': a['distance']}
                           for a in active]
            result = trilaterate(anchor_list)
            if result:
                target_position = result
                print(f"[SERVER] Target -> lat:{result['lat']:.6f} "
                      f"lon:{result['lon']:.6f} +/-{result['accuracy_m']}m")

    return jsonify({'status': 'ok', 'anchor_id': anchor_id}), 200


@app.route('/gps', methods=['POST'])
def receive_gps():
    """Phone app POSTs its GPS coordinates here."""
    global phone_position
    data = request.get_json()

    if not data:
        return jsonify({'error': 'No data'}), 400

    if 'lat' not in data or 'lon' not in data:
        return jsonify({'error': 'Missing lat/lon'}), 400

    with lock:
        phone_position = {
            'lat': data['lat'],
            'lon': data['lon'],
            'timestamp': time.time()
        }
        print(f"[SERVER] Phone GPS -> lat:{data['lat']:.8f} lon:{data['lon']:.8f}")

    return jsonify({'status': 'ok'}), 200


@app.route('/fence-status', methods=['GET', 'POST'])
def fence_status():
    """Frontend POSTs geofence status; animal.ino GETs it."""
    global geofence_status

    if request.method == 'POST':
        data = request.get_json()
        if not data:
            return jsonify({'error': 'No data'}), 400
        with lock:
            geofence_status = {
                'out_of_bounds': bool(data.get('out_of_bounds', False)),
                'timestamp': time.time()
            }
            state = 'OUT' if geofence_status['out_of_bounds'] else 'IN'
            print(f"[SERVER] Geofence status -> {state}")
        return jsonify({'status': 'ok'}), 200

    # GET - animal.ino polls this
    with lock:
        return jsonify(geofence_status)


@app.route('/vitals', methods=['POST'])
def receive_vitals():
    """Animal collar POSTs sensor vitals here."""
    global animal_vitals
    data = request.get_json()
    if not data:
        return jsonify({'error': 'No data'}), 400

    with lock:
        animal_vitals = {
            'heartRate':   data.get('heartRate', -1),
            'spo2':        data.get('spo2', -1),
            'temperature': data.get('temperature', -1),
            'motion':      data.get('motion', 0),
            'timestamp':   time.time()
        }
    return jsonify({'status': 'ok'}), 200


@app.route('/state', methods=['GET'])
def get_state():
    """Browser polls this to get current positions."""
    with lock:
        anchor_list = [
            {
                'id':       aid,
                'lat':      a['lat'],
                'lon':      a['lon'],
                'rssi':     a['rssi'],
                'distance': a['distance'],
                'active':   (time.time() - a['timestamp']) < 3.0
            }
            for aid, a in anchors.items()
        ]
        # Phone goes stale after 5 seconds
        phone = None
        if phone_position and (time.time() - phone_position['timestamp']) < 5.0:
            phone = {'lat': phone_position['lat'], 'lon': phone_position['lon']}
        # Vitals go stale after 10 seconds
        vitals = None
        if animal_vitals and (time.time() - animal_vitals['timestamp']) < 10.0:
            vitals = {k: v for k, v in animal_vitals.items() if k != 'timestamp'}
        return jsonify({
            'anchors':  anchor_list,
            'target':   target_position,
            'phone':    phone,
            'vitals':   vitals,
            'geofence': geofence_status,
            'time':     time.time()
        })


@app.route('/stream')
def stream():
    """Server-Sent Events — pushes updates to browser in real time."""
    def generate():
        last_sent = None
        while True:
            time.sleep(0.5)
            with lock:
                anchor_list = [
                    {
                        'id':       aid,
                        'lat':      a['lat'],
                        'lon':      a['lon'],
                        'rssi':     a['rssi'],
                        'distance': a['distance'],
                        'active':   (time.time() - a['timestamp']) < 3.0
                    }
                    for aid, a in anchors.items()
                ]
                # Phone goes stale after 5 seconds
                phone = None
                if phone_position and (time.time() - phone_position['timestamp']) < 5.0:
                    phone = {'lat': phone_position['lat'], 'lon': phone_position['lon']}

                # Vitals go stale after 10 seconds
                vitals = None
                if animal_vitals and (time.time() - animal_vitals['timestamp']) < 10.0:
                    vitals = {k: v for k, v in animal_vitals.items() if k != 'timestamp'}

                payload = {
                    'anchors':  anchor_list,
                    'target':   target_position,
                    'phone':    phone,
                    'vitals':   vitals,
                    'geofence': geofence_status,
                    'time':     time.time()
                }

            data = json.dumps(payload)
            if data != last_sent:
                last_sent = data
                yield f"data: {data}\n\n"

    return Response(generate(), mimetype='text/event-stream')


@app.route('/')
def dashboard():
    return send_from_directory('.', 'index.html')


@app.route('/health')
def health():
    with lock:
        now = time.time()
        active_anchors = [
            aid for aid, a in anchors.items()
            if (now - a['timestamp']) < 3.0
        ]
        return jsonify({
            'status': 'running',
            'anchors': len(anchors),
            'active_anchor_ids': active_anchors
        })


if __name__ == '__main__':
    print("=" * 50)
    print("  ESP32 Geofence Trilateration Server")
    print("=" * 50)
    print(f"  Listening on http://0.0.0.0:5000")
    print(f"  Open index.html in your browser")
    print(f"  Health check: http://localhost:5000/health")
    print("=" * 50)
    app.run(host='0.0.0.0', port=5000, debug=False, threaded=True)
