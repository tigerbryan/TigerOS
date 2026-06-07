# TigerOS Firmware

TigerOS is a reusable ESP-IDF 5.x firmware framework for ESP32-S3 devices with 16MB or larger flash.

Current firmware line: `1.0.x` development.

## V1.0 Features

- V0.2 Web Console, login, token security, WiFi manager, local OTA, rollback safety, and ring-buffer logs remain intact.
- V0.3 optional MQTT cloud connectivity foundation remains intact.
- V0.4 BLE provisioning remains intact.
- V0.5 Home Assistant MQTT Discovery remains intact.
- V0.6 Cloud OTA check/download/install is implemented.
- V0.8 third-party BLE sensor gateway for Inkbird IBS-TH2 class sensors is implemented.
- V0.9 multi-brand BLE sensor parser registry supports Inkbird, ATC, BTHome, Xiaomi placeholders, and unknown raw debug packets.
- V0.9 Web Console i18n supports English and Simplified Chinese.
- V1.0 Universal Device Gateway adds a common child-device model, a reusable adapter interface, unified REST APIs, Web Console Devices page, normalized MQTT child-device state publishing, and optional adapter placeholders for MQTT, HTTP, Tasmota, ESPHome, Shelly, and generic devices.
- V1.1 hardware foundation adds protected Freenove Media Kit board status, TFT backlight control on GPIO20, Web Console Hardware page, and audio/camera/microphone driver placeholders.
- MQTT settings stored in NVS.
- MQTT starts only after WiFi is connected.
- Automatic reconnect handled by ESP-MQTT.
- Status publish every 30 seconds.
- Telemetry publish every 60 seconds.
- Command and OTA topics subscribed after connect.
- Last Will and Testament publishes offline status.
- Web Console MQTT settings panel.
- BLE provisioning using ESP-IDF NimBLE.
- BLE device advertises as `TigerOS-XXXX`, where `XXXX` is derived from the Bluetooth MAC address.
- Generic BLE clients can read device info, read provisioning status, and write WiFi credentials.
- Web Console BLE settings panel with enable/disable, pairing PIN, and optional proof-of-possession token.
- Home Assistant MQTT Discovery publishes retained entity config automatically.
- Home Assistant entities: restart button, RSSI sensor, heap sensor, uptime sensor, firmware version sensor, and LED switch placeholder.
- Cloud OTA uses HTTPS, bearer device token, SHA256 verification, OTA rollback safety, and scheduled daily checks.
- BLE sensor gateway passively scans advertisements, stores paired sensor MACs/location/bindkey placeholders in NVS, exposes latest readings in the Web Console, and publishes MQTT telemetry.

## Build

```bash
cd TigerOS/firmware
idf.py set-target esp32s3
idf.py build
```

## Flash

Replace `/dev/tty.usbmodemXXXX` with your board port.

```bash
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

Rollback support needs the V0.2+ bootloader. If the device was originally flashed with an older bootloader, do a full serial flash once before relying on OTA rollback.

## Default Login

- Username: `admin`
- Password: `tigeros`

The password is stored as a SHA-256 hash in NVS. V1.0 still treats this as a trusted-LAN development default. Change it before exposing a device beyond a trusted local network.

## Cloud OTA

Open the Web Console, log in, then use the Cloud OTA section.

Fields:

- Enable Cloud OTA
- Auto install forced/allowed updates
- OTA Server URL
- Channel: `stable` or `beta`
- Device Token

Device token is stored in NVS as the Tiger Cloud token placeholder and is never returned by `GET /api/ota/config`.

OTA check request sent by TigerOS:

```json
{
  "device_id": "...",
  "current_version": "1.0.0",
  "hardware_model": "esp32-s3",
  "channel": "stable",
  "flash_size": 16777216,
  "chip_model": "ESP32-S3"
}
```

Expected server response:

```json
{
  "update_available": true,
  "version": "0.7.1",
  "firmware_url": "https://example.com/TigerOS.bin",
  "sha256": "...",
  "release_notes": "...",
  "force": false
}
```

TigerOS downloads `firmware_url`, calculates SHA256 while streaming the binary into the OTA partition, sets the boot partition only after verification succeeds, then reboots through the existing rollback-safe flow.

Scheduled checks run once after boot when WiFi is ready, then every 24 hours while Cloud OTA is enabled. TigerOS installs automatically only when the server returns `force=true` or local `auto_update=true`.

## MQTT Configuration

Open the Web Console, log in, then use the MQTT section.

Fields:

- Enable MQTT
- Host
- Port
- Username
- Password
- Client ID
- Use TLS
- Home Assistant Discovery
- Discovery Prefix

The MQTT password is stored in NVS but is never returned by `GET /api/mqtt`. Leaving the password field blank keeps the saved password.

TLS is structurally prepared through `mqtts://`, but TigerOS does not yet include certificate provisioning. Production TLS should add broker CA certificate storage or certificate bundle configuration.

Home Assistant Discovery is enabled by default and uses the prefix `homeassistant`.

## MQTT Topics

TigerOS uses:

```text
tigeros/{device_id}/status
tigeros/{device_id}/telemetry
tigeros/{device_id}/command
tigeros/{device_id}/response
tigeros/{device_id}/log
tigeros/{device_id}/ota
```

Status payload:

```json
{
  "device_id": "...",
  "firmware": "1.0.0",
  "build_time": "...",
  "wifi_ssid": "...",
  "ip": "...",
  "uptime": 123,
  "free_heap": 123456,
  "rssi": -45,
  "online": true
}
```

## MQTT Commands

Publish commands to:

```text
tigeros/{device_id}/command
```

Responses are published to:

```text
tigeros/{device_id}/response
```

Supported commands:

```json
{"command":"get_status"}
```

```json
{"command":"reboot"}
```

```json
{"command":"factory_reset"}
```

```json
{"command":"control","target":"led","value":true}
```

```json
{"command":"ota_check"}
```

Response format:

```json
{
  "ok": true,
  "command": "get_status",
  "message": "Status published",
  "timestamp": 123
}
```

## Mosquitto Test Commands

Subscribe to status:

```bash
mosquitto_sub -h <broker> -t "tigeros/+/status" -v
```

Subscribe to responses:

```bash
mosquitto_sub -h <broker> -t "tigeros/+/response" -v
```

Request status:

```bash
mosquitto_pub -h <broker> -t "tigeros/<device_id>/command" -m '{"command":"get_status"}'
```

Reboot:

```bash
mosquitto_pub -h <broker> -t "tigeros/<device_id>/command" -m '{"command":"reboot"}'
```

## Home Assistant MQTT Discovery

TigerOS publishes retained MQTT discovery configuration to the `homeassistant` prefix when MQTT connects.

Published entities:

- Switch: TigerOS LED placeholder
- Button: Restart Device
- Sensor: RSSI
- Sensor: Free Heap
- Sensor: Uptime
- Sensor: Firmware Version

Discovery topics:

```text
homeassistant/sensor/{device_id}/rssi/config
homeassistant/sensor/{device_id}/heap/config
homeassistant/sensor/{device_id}/uptime/config
homeassistant/sensor/{device_id}/firmware/config
homeassistant/button/{device_id}/restart/config
homeassistant/switch/{device_id}/led/config
```

Detailed setup and example screenshots are in [Home Assistant MQTT Discovery](../docs/home-assistant-mqtt-discovery.md).

## REST API

Public:

- `GET /api/status`
- `POST /api/login`
- `GET /api/wifi/scan`

Protected APIs require:

```http
Authorization: Bearer <token>
```

Protected:

- `POST /api/wifi`
- `POST /api/reboot`
- `POST /api/factory-reset`
- `POST /api/ota`
- `GET /api/logs`
- `POST /api/control`
- `GET /api/ota/config`
- `POST /api/ota/config`
- `POST /api/ota/check`
- `POST /api/ota/update`
- `GET /api/mqtt`
- `POST /api/mqtt`
- `GET /api/ble`
- `POST /api/ble`
- `GET /api/ble-sensors/scan`
- `GET /api/ble-sensors/scan/status`
- `POST /api/ble-sensors/scan/start`
- `POST /api/ble-sensors/scan/stop`
- `GET /api/ble-sensors/discovered`
- `GET /api/ble-sensors/paired`
- `POST /api/ble-sensors/pair`
- `POST /api/ble-sensors/remove`
- `POST /api/ble-sensors/rename`
- `POST /api/ble-sensors/location`
- `POST /api/ble-sensors/bindkey`
- `GET /api/ble-sensors/latest`
- `GET /api/ble-sensors/raw`

## BLE Provisioning

TigerOS V0.4 exposes three BLE GATT services:

- Device Info: read firmware, build time, device ID, and BLE device name.
- WiFi Provisioning: write WiFi credentials and read provisioning status.
- Status: read current BLE and WiFi provisioning state.

Default BLE settings:

- Device name: `TigerOS-XXXX`
- Pairing PIN: `123456`
- Proof-of-possession token: disabled unless configured in the Web Console

Provisioning workflow with nRF Connect:

1. Open the Web Console and log in.
2. In the BLE Provisioning section, enable BLE and note the device name.
3. Optional: set a six-digit pairing PIN and a proof-of-possession token.
4. Open nRF Connect or another generic BLE client.
5. Scan for `TigerOS-XXXX` and connect.
6. Pair when prompted. The default PIN is `123456`.
7. Read the Device Info or Status characteristic to verify the device.
8. Write WiFi credentials to the WiFi Provisioning characteristic as UTF-8 JSON.

Without proof-of-possession:

```json
{"ssid":"YourSSID","password":"YourPassword"}
```

With proof-of-possession:

```json
{"ssid":"YourSSID","password":"YourPassword","pop":"YourToken"}
```

After a valid provisioning write, TigerOS stores the WiFi credentials in NVS and starts WiFi connection automatically. Read the Status characteristic or refresh the Web Console to confirm `wifi_connected` and `ip`.

BLE provisioning status JSON:

```json
{
  "enabled": true,
  "connected": true,
  "device_name": "TigerOS-1A2B",
  "state": "Provisioned",
  "provisioning_state": "Provisioned",
  "wifi_connected": true,
  "ip": "192.168.31.208",
  "pop_required": false
}
```

## BLE Sensor Gateway

TigerOS V0.9 adds a multi-brand BLE sensor gateway. This is independent from BLE WiFi provisioning.

- Device discovery uses passive BLE scan.
- Manual scans are started from the Web Console.
- Automatic watched-device scans run as short background windows while WiFi is connected.
- Paired sensor MAC/name/brand/model/protocol/location/bindkey allowlist is stored in NVS.
- Discovered readings are stored in RAM and include raw advertisement hex for parser debugging.

MQTT per-sensor telemetry topic:

```text
tigeros/{gateway_device_id}/ble/{sensor_mac}/telemetry
```

Aggregated readings are included in:

```text
tigeros/{gateway_device_id}/telemetry
```

More details are in [BLE Sensor Gateway](../docs/BLE_SENSOR_GATEWAY.md).

## Known Limitations

- No password-change UI yet.
- Local HTTP is not encrypted.
- MQTT TLS certificate management is a placeholder.
- Logs are RAM-only and reset on reboot.
- MQTT command authorization depends on broker security.
- MQTT OTA topic is subscribed, but full remote MQTT OTA orchestration is not implemented yet.
- Cloud OTA requires a valid HTTPS endpoint trusted by ESP-IDF certificate bundle.
- BLE provisioning uses a generic JSON-over-GATT workflow. A dedicated Tiger App is not included yet.
- BLE proof-of-possession is optional and should be enabled for production provisioning.
- Home Assistant Discovery is implemented, but advanced entity customization is not yet exposed in the Web Console.
- Tiger Cloud V1.0 is a foundation service, not a hardened production SaaS yet.
