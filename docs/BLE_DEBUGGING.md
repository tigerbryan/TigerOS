# TigerOS BLE Debugging

TigerOS reads Xiaomi/PVVX/ATC/BTHome sensors passively from BLE advertisements. A flashed Xiaomi thermometer does not need to stay connected to the ESP32-S3. It should broadcast data periodically, and TigerOS captures those packets during scan windows.

## Recommended Test Flow

1. Open the Web Console.
2. Go to `Data`.
3. Click `Clear Raw`.
4. Place the thermometer close to the ESP32-S3.
5. Click `Collect BLE`.
6. Wait for the 12 second scan window to finish.
7. Check `Raw BLE Packets` and `Collection Diagnostics`.

## What Good Data Looks Like

BTHome v2 usually appears as service data with UUID `0xFCD2`.

PVVX/ATC Xiaomi custom firmware may appear as Environmental Sensing service data with UUID `0x181A`.

Readable packets should show one or more of:

- `temperature_c`
- `humidity_percent`
- `battery_percent`
- `external_probe_temperature_c`

## Common Results

### Raw packet count is 0

The ESP32-S3 did not receive advertisements during the scan window.

Check:

- BLE provisioning advertising is not active at the same time as scanning.
- The thermometer is awake and nearby.
- The thermometer firmware is configured to advertise data.
- Try another scan after clearing raw packets.

### Raw packets appear, but only MAC/name is shown

TigerOS can hear the device, but the parser did not decode sensor values.

Check:

- `service_data_hex`
- `manufacturer_data_hex`
- `service_uuids`
- `parse_status`
- `debug`

If BTHome encryption is enabled, the packet may require a bindkey. Disable encryption for initial testing or add bindkey support for that sensor.

### Device is added but shows offline

Paired devices are considered live only after a recent matching advertisement is received. BLE sensors do not maintain a Bluetooth connection. Start a scan to refresh the latest state.

## API Endpoints

Protected endpoints require the Web Console bearer token.

```bash
curl -H "Authorization: Bearer <token>" http://<device-ip>/api/ble/stats
curl -H "Authorization: Bearer <token>" http://<device-ip>/api/ble/raw
curl -H "Authorization: Bearer <token>" http://<device-ip>/api/ble/discovered
curl -X POST -H "Authorization: Bearer <token>" http://<device-ip>/api/ble/raw/clear
curl -X POST -H "Authorization: Bearer <token>" http://<device-ip>/api/ble-sensors/scan/start
```

## Notes

- The raw packet history is an in-memory ring buffer capped at 100 packets.
- Nearby BLE devices can fill the buffer quickly, so clear it before focused testing.
- TigerOS keeps BLE WiFi provisioning and BLE sensor scanning separate because NimBLE can only run one GAP procedure at a time.
