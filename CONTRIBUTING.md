# Contributing to TigerOS

TigerOS is an open-source ESP32-S3 IoT gateway firmware framework. Contributions are welcome for firmware stability, device adapters, documentation, and cloud integration.

## Development Setup

Firmware:

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
```

Cloud:

```bash
cd cloud
cp .env.example .env
npm install
npm run prisma:generate
npm run build
```

## Contribution Guidelines

- Keep ESP32 firmware modules small and reusable.
- Prefer ESP-IDF native APIs over Arduino APIs.
- Do not commit local `.env` files, tokens, compiled firmware binaries, or build output.
- Add logs for failure cases that would be hard to debug on a deployed device.
- Keep memory usage in mind: BLE scan buffers, MQTT, HTTPS OTA, display, and camera work can compete for RAM on ESP32-S3.

## Pull Requests

Please include:

- What changed
- Why it changed
- How it was tested
- Any hardware or ESP-IDF version assumptions
