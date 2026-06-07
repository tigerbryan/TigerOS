# TigerOS

TigerOS is an open-source ESP-IDF 5.x firmware framework for ESP32-S3 IoT devices plus a TypeScript cloud foundation for fleet management and Cloud OTA.

> Status: early V1.x development foundation. The firmware is usable for lab and small-site deployments, but production users should review security defaults, hardware pin mappings, and OTA/cloud configuration before wide rollout.

## Project Introduction / 项目介绍

TigerOS is a free and open-source ESP32-S3 IoT gateway system. It helps makers, shops, small businesses, and hardware developers build reliable connected devices with WiFi setup, Web Console, OTA updates, BLE sensors, MQTT, Home Assistant discovery, and a cloud-ready device management foundation.

TigerOS 是一个免费开源的 ESP32-S3 物联网网关系统。它适合创客、门店、小型企业和硬件开发者，用来快速构建可靠的联网设备，内置 WiFi 配网、Web 控制台、OTA 升级、BLE 传感器采集、MQTT、Home Assistant 自动发现，以及可扩展的云端设备管理基础。

### More Languages

- English: Open-source ESP32-S3 firmware and cloud foundation for IoT gateways, sensor hubs, and connected devices.
- 简体中文: 面向 ESP32-S3 的开源物联网固件和云端基础，可用于网关、传感器中枢和智能设备。
- Español: Firmware ESP32-S3 y base cloud de código abierto para gateways IoT, sensores y dispositivos conectados.
- Français: Firmware ESP32-S3 open source et base cloud pour passerelles IoT, capteurs et appareils connectés.
- Deutsch: Open-Source-ESP32-S3-Firmware und Cloud-Grundlage für IoT-Gateways, Sensor-Hubs und vernetzte Geräte.
- Português: Firmware ESP32-S3 open source e base cloud para gateways IoT, sensores e dispositivos conectados.
- 日本語: ESP32-S3 向けのオープンソース IoT ゲートウェイファームウェアとクラウド基盤です。
- 한국어: ESP32-S3 기반 오픈소스 IoT 게이트웨이 펌웨어와 클라우드 기반입니다.
- العربية: نظام مفتوح المصدر لبوابات إنترنت الأشياء المبنية على ESP32-S3 مع دعم الحساسات والتحديثات السحابية.

## Architecture

```text
TigerOS/
├── firmware/   ESP-IDF firmware for ESP32-S3 devices
├── cloud/      Tiger Cloud Node.js/TypeScript service
├── docs/       Product notes and integration guides
└── hardware/   Hardware notes
```

## Firmware

Current firmware line: `1.0.x` development.

Implemented foundations:

- SoftAP setup and WiFi manager
- Embedded Web Console
- NVS storage
- Login and bearer-token-protected APIs
- Local OTA upload with rollback safety
- Cloud OTA check/download/install with SHA256 verification
- BLE provisioning
- MQTT connectivity
- Home Assistant MQTT Discovery
- Multi-brand BLE sensor gateway for Inkbird, Xiaomi/PVVX/ATC, BTHome, and unknown BLE debug packets
- Universal Device Gateway registry, adapter interface, unified REST API, and normalized MQTT child-device state topics
- Freenove ESP32-S3 Media Kit hardware foundation with protected display backlight control and hardware status APIs
- English / Simplified Chinese Web Console i18n
- Ring-buffer logs

Build:

```bash
cd TigerOS/firmware
idf.py set-target esp32s3
idf.py build
```

The latest firmware binary is generated at:

```text
TigerOS/firmware/build/TigerOS.bin
```

## Tiger Cloud

Current cloud foundation version: `1.0.0`.

Implemented foundations:

- Node.js + TypeScript + Express
- PostgreSQL + Prisma ORM
- JWT user auth
- Per-device token auth
- Device registration/list/detail
- Device heartbeat
- Firmware release metadata
- OTA check API
- Gateway child BLE sensor registry and telemetry history API
- Universal gateway child device registry, state history, capability, log, and control placeholder APIs
- Docker Compose for local PostgreSQL

Run locally:

```bash
cd TigerOS/cloud
cp .env.example .env
docker compose up -d
npm install
npm run prisma:generate
npm run prisma:migrate
npm run dev
```

## Cloud OTA Flow

1. Build `TigerOS.bin`.
2. Host the binary on HTTPS-accessible storage.
3. Create a Tiger Cloud firmware release with version, channel, firmware URL, SHA256, and release notes.
4. Register a device in Tiger Cloud and copy its `device_token`.
5. In the TigerOS Web Console, open Cloud OTA and set:
   - OTA Server URL: `https://<cloud-host>/api/ota/check`
   - Channel: `stable` or `beta`
   - Device Token: token returned by Tiger Cloud
6. Click Check Update.
7. Click Update Now.

Cloud OTA uses the existing ESP-IDF OTA rollback flow. The new image is marked valid only after TigerOS boots far enough to initialize NVS, WiFi mode, Web Console, auth, and logs.

## Universal Device Gateway

TigerOS V1.0 turns the ESP32-S3 into a lightweight local gateway. Third-party devices can be normalized into one common child-device model instead of being hardcoded as temperature sensors.

- Web Console section: **Devices**
- Unified firmware APIs: `/api/devices`, `/api/devices/discovered`, `/api/devices/:id/state`, `/api/devices/:id/control`
- Supported V1.0 adapters: BLE sensor gateway plus lightweight placeholders for MQTT, HTTP, Tasmota, ESPHome, Shelly, and generic devices
- Normalized MQTT topic: `tigeros/{gateway_id}/devices/{device_id}/state`
- Tiger Cloud APIs: `/api/gateways/:gateway_id/devices`

Details are in [Universal Device Gateway](docs/UNIVERSAL_DEVICE_GATEWAY.md).

## BLE Sensor Gateway

TigerOS can scan and pair multi-brand BLE temperature and humidity sensors. V1.0 exposes them both through the legacy BLE sensor APIs and the new Universal Device Gateway model.

- Web Console section: **BLE Sensors**
- Passive BLE advertisement scanning for watched sensors
- Current auto-scan profile: short scan windows scheduled in the background while WiFi is connected
- Paired sensor allowlist is stored in NVS
- Supported protocols: Inkbird, ATC, BTHome, Xiaomi stock placeholder, unknown raw debug
- MQTT per-sensor topic: `tigeros/{gateway_device_id}/ble/{sensor_mac}/telemetry`
- Aggregated readings are included in `tigeros/{gateway_device_id}/telemetry`
- Tiger Cloud child sensor endpoints live under `/api/gateways/:gateway_id/ble-sensors`

Details are in [BLE Sensor Gateway](docs/BLE_SENSOR_GATEWAY.md).

## Security

Default Web Console login is `admin` / `tigeros`. This is a development default. Change passwords and API tokens before using TigerOS on any untrusted network.

See [Security Policy](SECURITY.md).

## License

TigerOS is released under the [MIT License](LICENSE), so anyone can use, modify, and distribute it for free, including commercial use.
