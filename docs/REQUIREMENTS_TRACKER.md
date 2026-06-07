# TigerOS Requirements Tracker

| Requirement | Status | Version | Notes |
|---|---|---:|---|
| Local Web Console | Complete | V0.1 | Embedded HTML/CSS/JS served from ESP-IDF HTTP server. |
| WiFi setup through Web Console | Complete | V0.1 | Saves SSID/password to NVS and reconnects. |
| WiFi network scan/select | Complete | V0.2 | `/api/wifi/scan` and Web Console dropdown. |
| NVS config storage | Complete | V0.1 | WiFi, device ID, auth token, cloud token, MQTT, OTA, BLE settings. |
| Local OTA upload | Complete | V0.2 | Streams uploaded `.bin` into OTA partition. |
| OTA rollback | Complete | V0.2 | Uses ESP-IDF rollback and marks app valid after successful boot init. |
| Remote Cloud OTA | Complete | V0.6 | HTTPS check/download, SHA256 verification, scheduled checks, rollback-safe install. |
| MQTT connectivity | Complete | V0.3 | ESP-IDF MQTT client with status, telemetry, command, response, log topics. |
| API control | Complete | V0.1 | Protected `/api/control` placeholder and MQTT `control` command. |
| BLE WiFi provisioning | Complete | V0.4 | NimBLE GATT provisioning with PIN and optional PoP. |
| Third-party BLE sensor gateway | Complete | V0.8 | Inkbird IBS-TH2 class passive scan, pairing allowlist, Web Console, REST APIs, MQTT telemetry, and Tiger Cloud child sensor model. |
| Multi-brand BLE sensor gateway | Complete | V0.9 | Parser registry supports Inkbird, ATC, BTHome, Xiaomi encrypted placeholder, unknown raw debug packets, location, bindkey placeholder, MQTT, and Cloud telemetry history. |
| Universal Device Gateway | Complete | V1.0 | Common child-device model, adapter interface, BLE/MQTT/HTTP/Tasmota/ESPHome/Shelly/generic adapter shells, unified REST API, Web Console Devices page, normalized MQTT output, and Tiger Cloud child-device APIs. |
| Open-source device integration | Partial | V1.0 | Universal adapter architecture exists; production-grade protocol-specific polling/control and OpenAPI/schema docs still needed. |
| Home Assistant integration | Complete | V0.5 | MQTT discovery for switch, button, RSSI, heap, uptime, firmware sensors. |
| Tiger Cloud device management | Partial | V0.7 | Register/list/detail and ownership checks exist; production RBAC/admin UI pending. |
| Device registration | Complete | V0.7 | User-authenticated registration returns per-device token. |
| Device heartbeat | Complete | V0.7 | Device-token-protected heartbeat updates last seen, online, IP, RSSI, heap, uptime. |
| Logs | Complete | V0.2 | In-memory ring buffer exposed through protected API and Web Console. |
| Factory reset | Complete | V0.7 | Clears WiFi, cloud, MQTT, OTA, Home Assistant, BLE provisioning config; preserves identity. |
| Security/login | Partial | V0.2 | Login/token protection exists; first-login password rotation and HTTPS are pending. |
| Documentation | Partial | V1.0 | README, review report, integration docs, BLE sensor guide, and Universal Device Gateway guide exist; OpenAPI and production hardening docs pending. |
