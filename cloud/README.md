# TigerOS Cloud

TigerOS Cloud V1.0 is the foundation for fleet management, Cloud OTA metadata, and universal gateway child devices. It is separate from the ESP-IDF firmware and lives under `TigerOS/cloud`.

## Stack

- Node.js
- TypeScript
- Express
- PostgreSQL
- Prisma ORM
- JWT user authentication
- Per-device bearer tokens
- MQTT-ready architecture placeholder
- Gateway child BLE sensor registry and telemetry history
- Universal child device registry, state history, capability, logs, and control placeholder APIs

## Local Setup

```bash
cd TigerOS/cloud
cp .env.example .env
docker compose up -d
npm install
npm run prisma:generate
npm run prisma:migrate
npm run dev
```

Health check:

```bash
curl http://localhost:8080/health
```

## Database Migration

```bash
npm run prisma:migrate
```

Open Prisma Studio:

```bash
npm run prisma:studio
```

## API Flow

Register a user:

```bash
curl -s http://localhost:8080/api/auth/register \
  -H "Content-Type: application/json" \
  -d '{"email":"admin@example.com","password":"change-me-now","name":"Admin"}'
```

Login:

```bash
curl -s http://localhost:8080/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"email":"admin@example.com","password":"change-me-now"}'
```

Register a device with the returned JWT:

```bash
curl -s http://localhost:8080/api/devices/register \
  -H "Authorization: Bearer <jwt>" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"tiger-001","name":"Bench ESP32-S3","hardware_model":"esp32-s3","firmware_version":"1.0.0","channel":"stable"}'
```

Create a firmware release:

```bash
curl -s http://localhost:8080/api/firmware/releases \
  -H "Authorization: Bearer <jwt>" \
  -H "Content-Type: application/json" \
  -d '{
    "version":"0.7.1",
    "channel":"stable",
    "hardware_model":"esp32-s3",
    "firmware_url":"https://example.com/TigerOS-0.7.1.bin",
    "sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "release_notes":"Cloud OTA smoke test release",
    "force_update":false
  }'
```

Check OTA with the device token:

```bash
curl -s http://localhost:8080/api/ota/check \
  -H "Authorization: Bearer <device_token>" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"tiger-001","current_version":"1.0.0","hardware_model":"esp32-s3","channel":"stable"}'
```

Heartbeat with the device token:

```bash
curl -s http://localhost:8080/api/devices/heartbeat \
  -H "Authorization: Bearer <device_token>" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"tiger-001","firmware_version":"1.0.0","ip":"192.168.31.208","rssi":-45,"free_heap":270000,"uptime":1234}'
```

Register a child BLE sensor under a gateway:

```bash
curl -s http://localhost:8080/api/gateways/tiger-001/ble-sensors \
  -H "Authorization: Bearer <jwt>" \
  -H "Content-Type: application/json" \
  -d '{"mac":"AA:BB:CC:DD:EE:FF","name":"Fridge Sensor","brand":"xiaomi","model":"LYWSD03MMC","protocol":"bthome","location":"Fridge"}'
```

List child BLE sensors:

```bash
curl -s http://localhost:8080/api/gateways/tiger-001/ble-sensors \
  -H "Authorization: Bearer <jwt>"
```

Post child BLE telemetry with the gateway device token:

```bash
curl -s http://localhost:8080/api/gateways/tiger-001/ble-sensors/telemetry \
  -H "Authorization: Bearer <device_token>" \
  -H "Content-Type: application/json" \
  -d '{"sensors":[{"mac":"AA:BB:CC:DD:EE:FF","name":"Fridge Sensor","brand":"xiaomi","model":"LYWSD03MMC","protocol":"bthome","location":"Fridge","rssi":-61,"temperature_c":4.12,"humidity_percent":55.2,"battery_percent":92}]}'
```

Register a universal child device:

```bash
curl -s http://localhost:8080/api/gateways/tiger-001/devices \
  -H "Authorization: Bearer <jwt>" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"ble-aabbccddeeff","name":"Cat Room Temperature","type":"sensor","brand":"xiaomi","model":"LYWSD03MMC","protocol":"ble","address":"AA:BB:CC:DD:EE:FF","capabilities":["temperature","humidity","battery"]}'
```

Post normalized child device state with the gateway device token:

```bash
curl -s http://localhost:8080/api/gateways/tiger-001/devices/ble-aabbccddeeff/state \
  -H "Authorization: Bearer <device_token>" \
  -H "Content-Type: application/json" \
  -d '{"online":true,"state":{"temperature_c":24.5,"humidity_percent":55.2,"battery_percent":86},"capabilities":["temperature","humidity","battery"]}'
```

## OTA Test Flow

1. Build firmware and host `TigerOS.bin` on HTTPS storage.
2. Calculate SHA256:

```bash
shasum -a 256 TigerOS.bin
```

3. Create a firmware release with the binary URL and SHA256.
4. Configure TigerOS Web Console Cloud OTA URL:

```text
https://<cloud-host>/api/ota/check
```

5. Set channel to `stable` or `beta`.
6. Click Check Update, then Update Now.

## Security Notes

- User APIs require JWT bearer tokens.
- Device APIs require per-device bearer tokens.
- Device tokens are hashed before storage.
- Firmware binary upload/storage is a placeholder in V1.0.
- MQTT broker integration is intentionally prepared but not implemented yet.
