# TigerOS BLE Sensor Gateway

TigerOS can act as an ESP32-S3 BLE gateway for third-party temperature and humidity sensors. Sensors do not connect to WiFi; TigerOS scans BLE advertisements, parses readings, displays them in the Web Console, and forwards paired sensors through MQTT and Tiger Cloud.

## BLE Provisioning vs BLE Sensor Scanning

- BLE provisioning is a GATT service exposed by TigerOS for sending WiFi credentials to the ESP32-S3.
- BLE sensor scanning is passive scanning of nearby sensor advertisements.
- They share the same ESP-IDF NimBLE host but are separate TigerOS modules.
- If BLE is busy advertising or connected for provisioning, a sensor scan can fail and TigerOS retries later.

## Supported Sensor Protocols

- Inkbird IBS-TH2 / IBS-TH2 Plus: manufacturer data parser.
- Xiaomi LYWSD03MMC / MHO style devices flashed with PVVX or ATC firmware:
  - ATC custom format parser.
  - BTHome parser.
  - PVVX custom format placeholder where raw packets are retained.
- Xiaomi stock encrypted format placeholder:
  - Encrypted packets are marked `parse_status: encrypted`.
  - Bindkey is stored as a placeholder for future decryption.
- Unknown BLE sensors:
  - Stored with raw advertisement hex for debugging.

Recommended Xiaomi firmware broadcast mode: **BTHome** or **ATC**. These are easier to parse and integrate than encrypted stock Xiaomi frames.

## Web Console Workflow

1. Log in to the TigerOS Web Console.
2. Open **BLE Sensors**.
3. Click **Scan Devices**.
4. Review discovered sensors:
   - Brand
   - Model
   - Protocol
   - MAC
   - BLE name
   - RSSI
   - Temperature
   - Humidity
   - Battery
   - Parse status
   - Raw packet
5. Click **Add Sensor** to pair it.
6. Rename it and choose a location:
   - Fridge
   - Freezer
   - Greenhouse
   - Cat Room
   - Reptile Box
   - Custom
7. Only paired sensors are published to MQTT by default.

## REST API

Protected endpoints require:

```http
Authorization: Bearer <web-console-token>
```

```text
GET  /api/ble-sensors/scan/status
POST /api/ble-sensors/scan/start
POST /api/ble-sensors/scan/stop
GET  /api/ble-sensors/discovered
GET  /api/ble-sensors/paired
POST /api/ble-sensors/pair
POST /api/ble-sensors/remove
POST /api/ble-sensors/rename
POST /api/ble-sensors/location
POST /api/ble-sensors/bindkey
GET  /api/ble-sensors/latest
GET  /api/ble-sensors/raw
```

Pair payload:

```json
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "name": "Cat Room",
  "brand": "xiaomi",
  "model": "LYWSD03MMC",
  "protocol": "bthome",
  "location": "Cat Room"
}
```

Bindkey placeholder:

```json
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "bindkey": "00112233445566778899AABBCCDDEEFF"
}
```

## MQTT

Per paired sensor:

```text
tigeros/{gateway_device_id}/ble/{sensor_mac}/telemetry
```

Example payload:

```json
{
  "gateway_device_id": "tiger-001",
  "sensor_mac": "AA:BB:CC:DD:EE:FF",
  "sensor_name": "Cat Room",
  "brand": "xiaomi",
  "model": "LYWSD03MMC",
  "protocol": "bthome",
  "temperature_c": 24.5,
  "humidity_percent": 55.2,
  "battery_percent": 86,
  "rssi": -65,
  "last_seen": 123456
}
```

Gateway aggregate:

```text
tigeros/{gateway_device_id}/telemetry
```

Includes:

- `paired_ble_sensor_count`
- `discovered_ble_sensor_count`
- `ble_sensors`

Mosquitto:

```bash
mosquitto_sub -h <broker> -t "tigeros/+/ble/+/telemetry" -v
mosquitto_sub -h <broker> -t "tigeros/+/telemetry" -v
```

## Tiger Cloud API

User JWT endpoints:

```text
GET    /api/gateways/:gateway_id/ble-sensors
POST   /api/gateways/:gateway_id/ble-sensors
PATCH  /api/gateways/:gateway_id/ble-sensors/:sensor_id
DELETE /api/gateways/:gateway_id/ble-sensors/:sensor_id
GET    /api/gateways/:gateway_id/ble-sensors/:sensor_id/telemetry/latest
```

Device-token endpoints:

```text
POST /api/gateways/:gateway_id/ble-sensors/telemetry
POST /api/gateways/:gateway_id/ble-sensors/:sensor_id/telemetry
```

## nRF Connect Testing

1. Open nRF Connect.
2. Scan nearby BLE devices.
3. Look for names such as `sps`, `ATC_...`, `LYWSD03MMC`, `MHO...`, or BTHome service UUID `FCD2`.
4. Copy advertisement/service/manufacturer data.
5. Compare it with TigerOS Web Console **Raw Packet**.

If a sensor shows `encrypted`, switch Xiaomi/PVVX firmware to BTHome or ATC broadcast mode when possible. Otherwise save the bindkey placeholder and keep the raw packet for future decryption support.
