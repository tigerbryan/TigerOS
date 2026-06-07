# Codex for OSS Application Draft

This file contains draft answers for the OpenAI Codex for OSS application form.

## Project

- Project name: TigerOS
- Repository: https://github.com/tigerbryan/TigerOS
- License: MIT
- Primary maintainer: Bryan Chen / tigerbryan
- Project status: Early V1.x open-source foundation

## Short Description

TigerOS is a free and open-source ESP32-S3 IoT gateway firmware framework and cloud foundation for connected devices, BLE sensors, MQTT, OTA updates, Home Assistant integration, and small-business hardware deployments.

## Why TigerOS Matters

TigerOS helps makers, small businesses, and hardware developers build reusable local IoT gateways instead of relying on closed vendor hubs. It combines ESP-IDF firmware, WiFi provisioning, local Web Console, secure OTA, BLE sensor collection, MQTT, Home Assistant discovery, and a TypeScript cloud foundation into one reusable open-source project.

The project is especially useful for low-cost ESP32-S3 deployments such as food-safety temperature monitoring, BLE sensor gateways, small-shop automation, and open hardware experimentation. It is designed to normalize different device protocols into one common device model so future contributors can add adapters for BLE, MQTT, HTTP, Tasmota, ESPHome, Shelly, BTHome, Xiaomi/PVVX, Inkbird, and other open devices.

## Open Source Maintenance Work

- Firmware architecture and ESP-IDF module design
- BLE sensor parsing and gateway diagnostics
- OTA rollback, local OTA, and future Cloud OTA work
- Web Console UX and bilingual i18n
- MQTT and Home Assistant integration
- Documentation, security notes, and contribution templates
- Cloud foundation for future device management

## How Codex Would Help

Codex would help maintain TigerOS by speeding up firmware review, ESP-IDF debugging, BLE parser implementation, OTA safety checks, security review, documentation, and adapter development. It would also help contributors understand the architecture and create safer pull requests for new hardware profiles and device integrations.

## Suggested Application Statement

TigerOS is an open-source ESP32-S3 IoT gateway framework for low-cost local device management. It supports WiFi provisioning, OTA updates, BLE sensor collection, MQTT, Home Assistant discovery, and cloud-ready device APIs. It is designed to help makers and small businesses build reusable open hardware gateways instead of closed vendor hubs. Codex would help maintain firmware quality, improve BLE parser support, review security-sensitive OTA and auth code, and make the project easier for new contributors.

## Notes Before Submitting

- Add at least one GitHub Release before submitting.
- Keep repository public.
- Add roadmap and issue templates.
- Add several public issues that show active maintenance priorities.
- Do not paste private tokens or deployment credentials into the form.

