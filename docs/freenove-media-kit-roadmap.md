# Freenove Media Kit Roadmap

Target board: Freenove Media Kit for ESP32-S3 with camera, 3.5 inch display, microphone, speaker, SD card, and navigation input.

## Phase 1: Product Console

- OTA drag and drop upload
- WiFi scan and guided provisioning
- Better OTA success, failure, reboot, and version feedback
- Device health fields: flash size, PSRAM status, reset reason, RSSI, uptime, heap

## Phase 2: Board Profile

- Add `board_profile` module
- Detect and expose Freenove Media Kit capabilities
- Add `/api/capabilities`
- Store board model in NVS
- Add config switches for camera, display, audio, SD card, and input

## Phase 3: Local Display UI

- Add `display_manager`
- Show boot status, WiFi state, IP address, QR setup code, OTA progress, and errors
- Add simple on-device menu controlled by navigation buttons

## Phase 4: Camera

- Add `camera_manager`
- Add `/api/camera/status`
- Add `/api/camera/snapshot`
- Add Web Console preview
- Prepare future OpenAI Vision hook

## Phase 5: Audio

- Add `audio_manager`
- Add microphone capture diagnostics
- Add speaker test endpoint
- Prepare STT/TTS streaming hooks

## Phase 6: AI Device Layer

- Device registration API
- MQTT command channel
- HTTPS OTA manifest check
- Home Assistant discovery
- OpenAI/XiaoZhiAI bridge experiments
