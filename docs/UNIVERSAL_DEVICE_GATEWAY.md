# TigerOS Universal Device Gateway

TigerOS V1.0 makes the ESP32-S3 a lightweight local gateway instead of a single-purpose sensor firmware. The gateway discovers or pairs nearby/open devices, normalizes them into one common child-device model, exposes them through REST and MQTT, and lets Tiger Cloud store long-term state.

## Concept

There are two device layers:

- Gateway device: the ESP32-S3 running TigerOS. It owns WiFi, Web Console, MQTT, OTA, Cloud token, and local adapter scheduling.
- Child device: a third-party or local device represented by TigerOS, such as a BLE sensor, Tasmota plug, Shelly relay, ESPHome node, or generic HTTP endpoint.

TigerOS keeps the gateway lightweight. It should discover, normalize, control simple capabilities, and publish state. Long-term storage, heavy analytics, dashboards, and fleet management belong in MQTT consumers or Tiger Cloud.

## Common Device Model

Firmware and cloud use this normalized shape:

```json
{
  "id": "ble-aabbccddeeff",
  "name": "Cat Room Temperature",
  "type": "sensor",
  "brand": "xiaomi",
  "model": "LYWSD03MMC",
  "protocol": "ble",
  "address": "AA:BB:CC:DD:EE:FF",
  "location": "Cat Room",
  "online": true,
  "last_seen": 123456,
  "capabilities": ["temperature", "humidity", "battery"],
  "state": {
    "temperature_c": 24.5,
    "humidity_percent": 55.2,
    "battery_percent": 86
  },
  "raw": {}
}
```

Recommended type values are `sensor`, `switch`, `light`, `button`, `climate`, `lock`, `camera`, `robot`, and `unknown`.

Recommended protocol values are `ble`, `mqtt`, `http`, `local_api`, and `unknown`.

## Supported Protocols

V1.0 includes the full registry and adapter architecture. The BLE sensor adapter is active because it builds on the existing V0.9 BLE sensor gateway. Other adapters are intentionally lightweight placeholders so they do not allocate memory until their protocol is enabled in a later release.

| Adapter | V1.0 Status | Notes |
|---|---|---|
| BLE sensor | Active | Uses existing Inkbird, ATC, BTHome, Xiaomi/PVVX placeholder, and unknown BLE parser registry. |
| MQTT device | Skeleton | Prepared for Home Assistant discovery and generic MQTT device topics. |
| HTTP device | Skeleton | Manual add by IP is stored as a generic HTTP child device. |
| Tasmota | Skeleton | Detection target: `http://<ip>/cm?cmnd=Status%200`. |
| ESPHome | Skeleton | Prepared for future native/API or REST-style integration. |
| Shelly | Skeleton | Prepared for Shelly RPC/status endpoint probing. |
| Generic | Skeleton | Fallback for future plugin adapters and manually described APIs. |

## Adapter Interface

All adapters implement the same lightweight interface in firmware:

- `scan()`
- `pair()`
- `remove()`
- `poll()`
- `parse()`
- `get_state()`
- `set_state()`
- `publish()`
- `get_capabilities()`

Adapters should be optional. If a protocol is disabled, it should avoid creating clients, scan tasks, timers, buffers, or TLS state. The ESP32-S3 has enough power for gateway glue logic, but not for acting as a heavy local server.

## Firmware REST API

Protected APIs:

```text
GET  /api/devices
GET  /api/devices/discovered
POST /api/devices/scan
POST /api/devices/pair
POST /api/devices/remove
POST /api/devices/rename
POST /api/devices/location
GET  /api/devices/:id
GET  /api/devices/:id/state
POST /api/devices/:id/control
GET  /api/devices/:id/raw
```

Example control payload:

```json
{
  "capability": "switch",
  "value": true
}
```

In V1.0, BLE sensors expose state and raw data. Control returns a clear unsupported response until a controllable adapter implements `set_state()`.

## MQTT Output

TigerOS publishes normalized state for paired child devices:

```text
tigeros/{gateway_id}/devices/{device_id}/state
```

Payload:

```json
{
  "gateway_id": "tiger-80B54ED3A474",
  "device_id": "ble-aabbccddeeff",
  "name": "Cat Room Temperature",
  "type": "sensor",
  "brand": "xiaomi",
  "protocol": "ble",
  "capabilities": ["temperature", "humidity", "battery"],
  "state": {
    "temperature_c": 24.5,
    "humidity_percent": 55.2,
    "battery_percent": 86
  },
  "last_seen": 123456
}
```

Future controllable adapters should subscribe or bridge commands from:

```text
tigeros/{gateway_id}/devices/{device_id}/set
```

## Web Console

The Web Console now has a Devices section:

- Paired devices with type, brand, protocol, online state, latest values, and remove action.
- Discovered devices from BLE scans with add action.
- Add HTTP device by IP for manual generic HTTP/Tasmota/Shelly probing later.

The legacy BLE Sensors page remains available so existing V0.9 workflows keep working.

## Tiger Cloud

Tiger Cloud V1.0 extends gateways beyond BLE-only child sensors:

- `ChildDevice`
- `DeviceState`
- `DeviceCapability`
- `DeviceLog`

APIs:

```text
GET    /api/gateways/:gateway_id/devices
POST   /api/gateways/:gateway_id/devices
PATCH  /api/gateways/:gateway_id/devices/:device_id
DELETE /api/gateways/:gateway_id/devices/:device_id
POST   /api/gateways/:gateway_id/devices/:device_id/state
POST   /api/gateways/:gateway_id/devices/:device_id/control
GET    /api/gateways/:gateway_id/devices/:device_id/logs
```

User APIs require JWT. Device state ingestion requires the gateway device token.

## Examples

### Inkbird BLE Sensor

1. Open Web Console.
2. Start BLE scan from Devices.
3. Pair the discovered Inkbird device.
4. TigerOS maps temperature, humidity, battery, and optional external probe readings into the common state model.

### Xiaomi/PVVX or BTHome Sensor

1. Flash or configure the sensor for ATC/BTHome advertisements when possible.
2. Scan and pair from Devices.
3. TigerOS records the parser protocol in `raw.parser_protocol` and exposes normalized temperature, humidity, and battery capabilities when present.

Encrypted Xiaomi stock payloads remain a placeholder until bindkey decryption is completed.

### Tasmota Smart Plug

1. Add the plug by IP from Devices.
2. V1.0 stores it as a universal HTTP/generic child device.
3. Future adapter work should probe `http://<ip>/cm?cmnd=Status%200`, map relay state to `power`, and implement `set_state("switch", true/false)`.

### Shelly Relay

1. Add the relay by IP from Devices.
2. Future adapter work should detect Shelly RPC/status endpoints, map relay state to `power`, and expose switch capability.

### Generic HTTP API Device

1. Add by IP or future manual endpoint configuration.
2. Store as `protocol: "http"` and `brand: "generic"`.
3. A custom adapter can later parse the response into the common `state` object.

## Adding A New Adapter

1. Add a new adapter class under `firmware/main/adapters/`.
2. Implement only the methods needed for the protocol.
3. Keep scan/poll tasks disabled unless the adapter is enabled by config.
4. Normalize output into `UniversalDevice`.
5. Store pairing/config data in NVS through a compact schema.
6. Publish state through `DeviceRegistry::publish_state()`.
7. Add Web Console controls only for capabilities that are safe and implemented.
8. Add cloud examples if the device needs server-side state history or control logs.

## Limits

- ESP32-S3 memory is finite. Avoid loading all protocol stacks at once.
- TLS, BLE scan buffers, MQTT queues, and camera/display workloads can compete for RAM.
- Use short scan windows and small ring buffers.
- Keep raw payload retention small.
- Do not use TigerOS as a database. Publish to MQTT or Tiger Cloud for history.
- Local control should be best-effort and quick; slow retries belong in protocol-specific tasks.
