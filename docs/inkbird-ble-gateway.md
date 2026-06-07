# TigerOS V0.8 Inkbird BLE Sensor Gateway

TigerOS V0.8 adds a third-party BLE sensor gateway mode for Inkbird IBS-TH2 / IBS-TH2 Plus style temperature and humidity sensors. This is separate from BLE WiFi provisioning.

## Workflow

1. Power on the Inkbird sensor near the ESP32-S3 gateway.
2. Open the TigerOS Web Console and log in.
3. Open **BLE Sensors**.
4. Click **Scan**.
5. When a sensor appears, inspect RSSI, parsed values, and raw advertisement.
6. Click **Pair** to store the sensor MAC in NVS.
7. Rename the paired sensor if needed.

TigerOS keeps discovered sensors in RAM and paired sensors in NVS. Factory reset clears the paired sensor list.

## BLE Scanning

- Passive scan only.
- Default scan window: 5 seconds.
- Default automatic interval: 30 seconds while WiFi is connected.
- The sensor gateway reuses the existing ESP-IDF NimBLE host used by BLE provisioning.
- If provisioning advertising or another BLE operation is busy, scan start can fail and TigerOS retries later.

## Inkbird Packet Notes

Many IBS-TH2 class devices advertise the local name `sps`. TigerOS looks for that name and attempts to parse manufacturer data containing temperature, humidity, battery, and internal/external probe flags.

Because Inkbird packets can vary by firmware revision, TigerOS always exposes raw advertisement hex in the Web Console so unsupported variants can be decoded later.

## REST API

All BLE sensor APIs require:

```http
Authorization: Bearer <web-console-token>
```

Endpoints:

```text
GET  /api/ble-sensors/scan
POST /api/ble-sensors/scan/start
POST /api/ble-sensors/scan/stop
GET  /api/ble-sensors/discovered
GET  /api/ble-sensors/paired
POST /api/ble-sensors/pair
POST /api/ble-sensors/remove
POST /api/ble-sensors/rename
GET  /api/ble-sensors/latest
```

## MQTT Telemetry

Per-sensor telemetry:

```text
tigeros/{gateway_device_id}/ble/{sensor_mac}/telemetry
```

Gateway aggregated telemetry:

```text
tigeros/{gateway_device_id}/telemetry
```

Example sensor payload:

```json
{
  "gateway_device_id": "tiger-001",
  "mac": "AA:BB:CC:DD:EE:FF",
  "display_name": "Fridge Sensor",
  "sensor_type": "inkbird_ibsth2",
  "rssi": -61,
  "last_seen": 123,
  "temperature_c": 4.12,
  "humidity_percent": 55.2,
  "battery_percent": 92
}
```

## Tiger Cloud Child Sensors

Tiger Cloud V0.8 adds child BLE sensors under a gateway device.

User-managed endpoints:

```text
GET  /api/gateways/:gateway_id/ble-sensors
POST /api/gateways/:gateway_id/ble-sensors
```

Device-token telemetry endpoint:

```text
POST /api/gateways/:gateway_id/ble-sensors/telemetry
```

Telemetry payload:

```json
{
  "sensors": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "name": "Fridge Sensor",
      "sensor_type": "inkbird_ibsth2",
      "rssi": -61,
      "temperature_c": 4.12,
      "humidity_percent": 55.2,
      "battery_percent": 92
    }
  ]
}
```

## Troubleshooting

- If no sensors appear, move the sensor closer and click Scan again.
- If the name is `sps` but values are missing, copy the raw advertisement hex for parser improvement.
- If scanning does not start, BLE provisioning may currently be advertising or connected; TigerOS will retry automatically.
- If MQTT telemetry is missing, confirm MQTT is enabled and connected first.
