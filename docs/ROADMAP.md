# TigerOS Roadmap

TigerOS is an early open-source ESP32-S3 IoT gateway foundation. This roadmap keeps the project direction visible for contributors and users.

## Near Term

- Stabilize BLE sensor collection for Inkbird IBS-TH2 / IBS-TH2 Plus devices.
- Improve passive BLE scan power profiles and diagnostic logs.
- Add more BTHome, ATC/PVVX, Xiaomi, and Inkbird parser examples.
- Add documented device profiles for common ESP32-S3 boards.
- Improve Freenove Media Kit display/audio/camera support after pin mapping validation.
- Add GitHub Actions build validation for firmware and cloud components.
- Publish signed or checksumed firmware binaries through GitHub Releases.

## Medium Term

- Expand Universal Device Gateway adapters for Tasmota, ESPHome, Shelly, generic HTTP, and MQTT devices.
- Improve Home Assistant discovery for child gateway devices.
- Add better Cloud OTA fleet rollout controls.
- Add device registration and ownership flows for production deployments.
- Add better i18n coverage for Web Console and documentation.

## Long Term

- Build a lightweight plugin model for third-party device adapters.
- Support more commercial BLE sensors used in food safety, cold-chain monitoring, and home automation.
- Add optional edge rules for alerts and local automation.
- Provide hardened deployment guides for small shops, labs, and makerspaces.

## Good First Issues

- Add screenshots to documentation.
- Add parser test vectors for BLE advertisements.
- Improve setup copy for first-time WiFi onboarding.
- Add translations for missing Web Console strings.
- Document known hardware profiles and tested boards.

