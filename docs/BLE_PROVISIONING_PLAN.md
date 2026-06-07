# BLE Provisioning Plan

Note: TigerOS already includes an initial BLE provisioning implementation from V0.4. This plan documents the intended provisioning design and the next hardening steps without adding new BLE code in this stabilization pass.

## BLE Provisioning Flow

1. Device boots and initializes NVS, WiFi Manager, Web Console, and BLE Manager.
2. If BLE provisioning is enabled, device advertises as `TigerOS-XXXX`.
3. User scans with nRF Connect or a future Tiger App.
4. User connects and pairs using the configured six-digit PIN.
5. Optional proof-of-possession token is required if configured in Web Console.
6. Mobile client writes WiFi credentials as UTF-8 JSON to the provisioning characteristic.
7. BLE Manager validates JSON and PoP token.
8. BLE Manager calls WiFi Manager `save_and_connect()`.
9. WiFi Manager stores credentials in NVS and connects station mode.
10. BLE status characteristic reports provisioning state, WiFi status, and IP.

## ESP-IDF Components Needed

- `bt`
- NimBLE host
- NimBLE GAP service
- NimBLE GATT service
- NVS flash
- WiFi
- ESP event loop
- FreeRTOS tasks

Current sdkconfig assumptions:

- `CONFIG_BT_ENABLED=y`
- `CONFIG_BT_BLUEDROID_ENABLED=n`
- `CONFIG_BT_NIMBLE_ENABLED=y`
- `CONFIG_BT_NIMBLE_SECURITY_ENABLE=y`

## Security Method

Initial method:

- BLE pairing with six-digit PIN.
- Optional proof-of-possession token stored in NVS.
- Provisioning write characteristic requires encrypted write.

Production hardening:

- Generate per-device PIN or setup code at manufacturing/provisioning time.
- Hide pairing PIN from Web Console after first configuration, or require password re-entry to reveal.
- Add provisioning timeout window.
- Add failed-attempt backoff for PoP failures.
- Consider QR code containing device ID and one-time setup token.

## BLE Service and Characteristics Design

Device name:

```text
TigerOS-XXXX
```

Services:

| Service | Characteristic | Properties | Payload |
|---|---|---|---|
| Device Info | Device Info | Read | JSON: device_id, firmware, build_time, device_name |
| WiFi Provisioning | Provisioning Write | Encrypted Write | JSON: ssid, password, optional pop |
| WiFi Provisioning | Provisioning Status | Read, Notify | JSON: provisioning state, WiFi status, IP |
| Status | Status | Read, Notify | JSON: BLE enabled/connected/state |

Provisioning write payload:

```json
{
  "ssid": "ExampleWiFi",
  "password": "ExamplePassword",
  "pop": "optional-token"
}
```

## Phone App Compatibility

Supported now:

- Generic BLE clients
- nRF Connect

Future Tiger App:

- Scan for `TigerOS-XXXX`.
- Show device info before provisioning.
- Request PIN or setup QR code.
- Submit SSID/password/PoP.
- Poll or subscribe to provisioning status.
- Show final LAN IP and open Web Console.

## nRF Connect Testing Steps

1. Enable BLE in TigerOS Web Console.
2. Set pairing PIN and optional PoP token.
3. Open nRF Connect on iOS or Android.
4. Scan for `TigerOS-XXXX`.
5. Connect and pair with the configured PIN.
6. Read Device Info.
7. Write provisioning JSON to the WiFi Provisioning write characteristic.
8. Read Provisioning Status.
9. Confirm WiFi connection in the Web Console or device logs.
10. Reboot and confirm credentials persisted in NVS.

## Interaction With WiFi Manager and NVS

- BLE Manager should not own WiFi connection logic.
- BLE Manager validates provisioning payload and delegates to `wifi_manager().save_and_connect()`.
- WiFi Manager validates SSID/password length and persists credentials through NVS Store.
- NVS Store remains the single storage layer for WiFi credentials, BLE settings, PoP token, and pairing PIN.
- Factory reset clears BLE provisioning settings and WiFi credentials through `factory_reset_config()`.

## Next Hardening Tasks

- Add characteristic UUID documentation with exact UUID values.
- Add notify support when provisioning status changes.
- Add BLE provisioning timeout.
- Add per-device setup code generation.
- Add BLE-specific logs for successful pair/write/status reads.
- Add hardware test matrix for Freenove ESP32-S3 CAM/display board and plain ESP32-S3 devkits.
