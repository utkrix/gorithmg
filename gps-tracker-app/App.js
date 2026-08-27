import React, { useState, useEffect, useRef, useCallback } from 'react';
import {
  StyleSheet,
  Text,
  View,
  TouchableOpacity,
  StatusBar,
  Animated,
  Platform,
} from 'react-native';
import * as Location from 'expo-location';

// ── Config ───────────────────────────────────────────────────────────────────
const SERVER_URL = 'http://10.187.12.253:5000';
const SEND_INTERVAL_MS = 1000; // Send every 1 second

export default function App() {
  const [tracking, setTracking] = useState(false);
  const [location, setLocation] = useState(null);
  const [status, setStatus] = useState('idle'); // idle | requesting | tracking | error
  const [errorMsg, setErrorMsg] = useState(null);
  const [sendCount, setSendCount] = useState(0);
  const [lastResponse, setLastResponse] = useState(null);

  const locationSubscription = useRef(null);
  const sendInterval = useRef(null);
  const latestLocation = useRef(null);
  const pulseAnim = useRef(new Animated.Value(1)).current;

  // ── Pulse animation for the live dot ──
  useEffect(() => {
    if (tracking) {
      const loop = Animated.loop(
        Animated.sequence([
          Animated.timing(pulseAnim, {
            toValue: 1.6,
            duration: 800,
            useNativeDriver: true,
          }),
          Animated.timing(pulseAnim, {
            toValue: 1,
            duration: 800,
            useNativeDriver: true,
          }),
        ])
      );
      loop.start();
      return () => loop.stop();
    } else {
      pulseAnim.setValue(1);
    }
  }, [tracking]);

  // ── Send GPS data to server ──
  const sendLocation = useCallback(async () => {
    const loc = latestLocation.current;
    if (!loc) return;

    try {
      const response = await fetch(`${SERVER_URL}/gps`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'ngrok-skip-browser-warning': 'true',
        },
        body: JSON.stringify({
          lat: loc.coords.latitude,
          lon: loc.coords.longitude,
        }),
      });

      const data = await response.json();
      setLastResponse(data);
      setSendCount((prev) => prev + 1);
    } catch (err) {
      console.warn('Send failed:', err.message);
    }
  }, []);

  // ── Start tracking ──
  const startTracking = async () => {
    setErrorMsg(null);
    setStatus('requesting');

    const { status: permStatus } = await Location.requestForegroundPermissionsAsync();
    if (permStatus !== 'granted') {
      setErrorMsg('Location permission denied');
      setStatus('error');
      return;
    }

    setStatus('tracking');
    setTracking(true);
    setSendCount(0);

    // Watch position with high accuracy
    locationSubscription.current = await Location.watchPositionAsync(
      {
        accuracy: Location.Accuracy.BestForNavigation,
        timeInterval: 500,
        distanceInterval: 0,
      },
      (loc) => {
        latestLocation.current = loc;
        setLocation(loc);
      }
    );

    // Send data at fixed interval
    sendInterval.current = setInterval(sendLocation, SEND_INTERVAL_MS);
  };

  // ── Stop tracking ──
  const stopTracking = () => {
    if (locationSubscription.current) {
      locationSubscription.current.remove();
      locationSubscription.current = null;
    }
    if (sendInterval.current) {
      clearInterval(sendInterval.current);
      sendInterval.current = null;
    }
    setTracking(false);
    setStatus('idle');
  };

  // ── Cleanup on unmount ──
  useEffect(() => {
    return () => {
      stopTracking();
    };
  }, []);

  const lat = location?.coords?.latitude;
  const lon = location?.coords?.longitude;
  const accuracy = location?.coords?.accuracy;

  return (
    <View style={styles.container}>
      <StatusBar barStyle="light-content" backgroundColor="#0a0f1a" />

      {/* ── Header ── */}
      <View style={styles.header}>
        <View style={styles.headerTop}>
          <View style={styles.badge}>
            <Text style={styles.badgeText}>GPS SENDER</Text>
          </View>
          <View style={styles.statusRow}>
            <Animated.View
              style={[
                styles.statusDot,
                tracking ? styles.statusDotLive : styles.statusDotOff,
                tracking && { transform: [{ scale: pulseAnim }] },
              ]}
            />
            <Text style={styles.statusLabel}>
              {tracking ? 'LIVE' : 'OFFLINE'}
            </Text>
          </View>
        </View>
        <Text style={styles.title}>Location Tracker</Text>
        <Text style={styles.subtitle}>{SERVER_URL}</Text>
      </View>

      {/* ── Coordinates Display ── */}
      <View style={styles.coordsCard}>
        <View style={styles.coordRow}>
          <Text style={styles.coordLabel}>LATITUDE</Text>
          <Text style={styles.coordValue}>
            {lat != null ? lat.toFixed(8) : '—'}
          </Text>
        </View>
        <View style={styles.divider} />
        <View style={styles.coordRow}>
          <Text style={styles.coordLabel}>LONGITUDE</Text>
          <Text style={styles.coordValue}>
            {lon != null ? lon.toFixed(8) : '—'}
          </Text>
        </View>
        <View style={styles.divider} />
        <View style={styles.coordRow}>
          <Text style={styles.coordLabel}>ACCURACY</Text>
          <Text style={styles.coordValue}>
            {accuracy != null ? `±${accuracy.toFixed(1)} m` : '—'}
          </Text>
        </View>
      </View>

      {/* ── Stats Row ── */}
      <View style={styles.statsRow}>
        <View style={styles.statCard}>
          <Text style={styles.statNumber}>{sendCount}</Text>
          <Text style={styles.statLabel}>PACKETS SENT</Text>
        </View>
        <View style={styles.statCard}>
          <Text style={styles.statNumber}>
            {SEND_INTERVAL_MS / 1000}s
          </Text>
          <Text style={styles.statLabel}>INTERVAL</Text>
        </View>
        <View style={styles.statCard}>
          <Text style={[styles.statNumber, { color: lastResponse?.status === 'ok' ? '#22c55e' : '#95a4b5' }]}>
            {lastResponse?.status === 'ok' ? 'OK' : '—'}
          </Text>
          <Text style={styles.statLabel}>SERVER</Text>
        </View>
      </View>

      {/* ── Error ── */}
      {errorMsg && (
        <View style={styles.errorBox}>
          <Text style={styles.errorText}>{errorMsg}</Text>
        </View>
      )}

      {/* ── Start/Stop Button ── */}
      <TouchableOpacity
        style={[styles.button, tracking ? styles.buttonStop : styles.buttonStart]}
        onPress={tracking ? stopTracking : startTracking}
        activeOpacity={0.8}
      >
        <View style={styles.buttonInner}>
          <View style={[styles.buttonIcon, tracking ? styles.stopIcon : styles.startIcon]} />
          <Text style={styles.buttonText}>
            {tracking ? 'STOP TRACKING' : 'START TRACKING'}
          </Text>
        </View>
      </TouchableOpacity>

      {/* ── Footer ── */}
      <Text style={styles.footer}>
        Sending to /gps endpoint • {Platform.OS.toUpperCase()}
      </Text>
    </View>
  );
}

// ── Styles ───────────────────────────────────────────────────────────────────
const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#0a0f1a',
    paddingHorizontal: 20,
    paddingTop: Platform.OS === 'android' ? 48 : 60,
  },
  header: {
    marginBottom: 28,
  },
  headerTop: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginBottom: 12,
  },
  badge: {
    backgroundColor: 'rgba(99, 102, 241, 0.2)',
    borderColor: 'rgba(99, 102, 241, 0.4)',
    borderWidth: 1,
    borderRadius: 999,
    paddingHorizontal: 10,
    paddingVertical: 5,
  },
  badgeText: {
    color: '#818cf8',
    fontSize: 11,
    fontWeight: '700',
    letterSpacing: 1.5,
  },
  statusRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
  statusDot: {
    width: 10,
    height: 10,
    borderRadius: 5,
  },
  statusDotLive: {
    backgroundColor: '#22c55e',
  },
  statusDotOff: {
    backgroundColor: '#ef4444',
  },
  statusLabel: {
    color: '#95a4b5',
    fontSize: 12,
    fontWeight: '600',
    letterSpacing: 1.2,
  },
  title: {
    color: '#e6f2ff',
    fontSize: 32,
    fontWeight: '800',
    letterSpacing: -0.5,
  },
  subtitle: {
    color: '#4b5563',
    fontSize: 13,
    marginTop: 4,
    fontFamily: Platform.OS === 'ios' ? 'Menlo' : 'monospace',
  },

  // Coordinates card
  coordsCard: {
    backgroundColor: 'rgba(255,255,255,0.04)',
    borderColor: 'rgba(255,255,255,0.08)',
    borderWidth: 1,
    borderRadius: 16,
    padding: 18,
    marginBottom: 16,
  },
  coordRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingVertical: 10,
  },
  coordLabel: {
    color: '#95a4b5',
    fontSize: 12,
    fontWeight: '700',
    letterSpacing: 1.2,
  },
  coordValue: {
    color: '#e6f2ff',
    fontSize: 18,
    fontWeight: '700',
    fontFamily: Platform.OS === 'ios' ? 'Menlo' : 'monospace',
  },
  divider: {
    height: 1,
    backgroundColor: 'rgba(255,255,255,0.06)',
  },

  // Stats
  statsRow: {
    flexDirection: 'row',
    gap: 10,
    marginBottom: 24,
  },
  statCard: {
    flex: 1,
    backgroundColor: 'rgba(255,255,255,0.03)',
    borderColor: 'rgba(255,255,255,0.06)',
    borderWidth: 1,
    borderRadius: 14,
    paddingVertical: 14,
    alignItems: 'center',
  },
  statNumber: {
    color: '#e6f2ff',
    fontSize: 22,
    fontWeight: '800',
  },
  statLabel: {
    color: '#4b5563',
    fontSize: 10,
    fontWeight: '600',
    letterSpacing: 1,
    marginTop: 4,
  },

  // Error
  errorBox: {
    backgroundColor: 'rgba(239, 68, 68, 0.15)',
    borderColor: 'rgba(239, 68, 68, 0.4)',
    borderWidth: 1,
    borderRadius: 12,
    padding: 14,
    marginBottom: 16,
  },
  errorText: {
    color: '#fca5a5',
    fontSize: 13,
    fontWeight: '500',
    textAlign: 'center',
  },

  // Button
  button: {
    borderRadius: 16,
    paddingVertical: 18,
    alignItems: 'center',
    marginTop: 'auto',
    marginBottom: 8,
  },
  buttonStart: {
    backgroundColor: '#22c55e',
  },
  buttonStop: {
    backgroundColor: '#ef4444',
  },
  buttonInner: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
  },
  buttonIcon: {
    width: 14,
    height: 14,
    borderRadius: 2,
  },
  startIcon: {
    borderRadius: 7,
    backgroundColor: 'rgba(255,255,255,0.3)',
  },
  stopIcon: {
    backgroundColor: 'rgba(255,255,255,0.3)',
  },
  buttonText: {
    color: '#fff',
    fontSize: 16,
    fontWeight: '800',
    letterSpacing: 1.5,
  },

  // Footer
  footer: {
    color: '#2a3442',
    fontSize: 12,
    textAlign: 'center',
    paddingVertical: 16,
  },
});
