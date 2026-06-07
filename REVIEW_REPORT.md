# TigerOS Review Report

Date: 2026-06-04

Scope reviewed:

- `firmware/`
- `firmware/main/`
- `firmware/web/`
- `firmware/partitions.csv`
- `firmware/sdkconfig.defaults`
- `cloud/`
- `docs/`
- top-level README files

## Critical Issues

### Fixed

- Cloud device registration allowed a user to submit an already registered `device_id` and update another user's device metadata. Fixed by checking ownership before update and returning `409` when a device belongs to another user.
- Cloud device re-registration returned a newly generated token without saving it on the update path. Fixed by explicitly rotating and storing the token hash when the owning user re-registers a device.

### Remaining

- None known after this pass.

## High Priority Issues

### Fixed

- Remote Cloud OTA accepted downloads without validating HTTP status, content length, partition capacity, or final byte count. Fixed by requiring 2xx response, positive content length, image size within the OTA partition, and exact downloaded byte count.
- Remote Cloud OTA SHA256 comparison rejected valid uppercase SHA256 strings and did not normalize server input. Fixed by lowercasing expected SHA256 before verification.
- Remote Cloud OTA shared progress and latest check state across web/API/scheduler tasks without locking. Fixed with a FreeRTOS mutex around check result and progress state.
- Factory reset cleared only WiFi and cloud token, leaving MQTT credentials, OTA config, BLE PoP/PIN, and Home Assistant discovery config in NVS. Fixed with `factory_reset_config()` to clear connectivity and provisioning secrets while preserving device identity.
- Cloud OTA version comparison used string inequality and selected newest release by creation time, allowing older versions to be offered as updates. Fixed by selecting the highest semantic-style version and only returning updates newer than the current version.
- Firmware auth token/hash comparisons used normal string equality. Fixed with fixed-time comparison to reduce timing leakage.
- Tiger Cloud could run in production with the development JWT secret. Fixed by requiring a 32+ character `JWT_SECRET` when `NODE_ENV=production`.

## Medium Priority Issues

- Web Console stores the local API token in `localStorage`. This is acceptable for a LAN console prototype but should move to short-lived sessions or hardened storage for production.
- Web Console is served over HTTP, not HTTPS. This is acceptable in SoftAP/LAN development but exposes credentials on untrusted networks.
- Firmware API still sets permissive `Access-Control-Allow-Origin: *`. Browser preflight limits most protected requests, but production should tighten CORS or remove it.
- Default Web Console password is documented and intentionally active on first boot. Production should force password rotation on first login.
- MQTT TLS support uses `mqtts://` structure but does not yet provide broker CA/certificate bundle configuration in the UI.
- BLE provisioning is implemented, but BLE pairing/PoP should receive device-specific production enrollment policy before release.
- Cloud firmware release creation is available to any authenticated user. A role/admin model is needed before multi-tenant production.
- Cloud does not yet implement binary upload/storage; release metadata points to externally hosted firmware.
- Cloud does not mark devices offline after heartbeat timeout; only heartbeat updates `online=true`.
- Cloud registration/login lacks rate limiting.
- Cloud version parser handles common numeric versions but does not implement full semver pre-release precedence.
- Device tokens are stored hashed, but device token rotation/revocation APIs are not yet exposed separately.

## Low Priority Issues

- `.DS_Store` files exist locally; `.gitignore` now excludes them.
- `firmware/sdkconfig` is generated and large. `sdkconfig.defaults` is the portable source of assumptions.
- BLE plan requested by this pass says not to implement BLE, but the repository already contains BLE provisioning from V0.4. The new plan documents hardening and validation rather than adding new code.
- README files are accurate at a foundation level, but API reference could be expanded into OpenAPI later.

## Recommended Fixes

Completed in this pass:

- Add Cloud device registration ownership checks.
- Add Cloud token rotation on owned device re-registration.
- Add production JWT secret guard.
- Add semantic-style OTA version comparison.
- Harden Cloud OTA HTTP/download/SHA validation.
- Add Cloud OTA state locking.
- Expand factory reset to clear all connectivity/provisioning secrets.
- Use fixed-time comparisons for local auth.
- Update factory reset Web Console copy.
- Add requirement tracker and BLE provisioning plan.

Recommended next:

- Add Cloud rate limiting and structured request IDs.
- Add role-based access control for firmware release creation.
- Add firmware binary upload/storage and signed release metadata.
- Add device offline timeout worker.
- Add first-login password change flow.
- Add MQTT broker CA certificate configuration.
- Add OpenAPI specs for firmware and cloud APIs.
- Add unit tests for Cloud version comparison and auth middleware.

## Files Changed

- `TigerOS/firmware/main/auth_manager.cpp`
- `TigerOS/firmware/main/api_routes.cpp`
- `TigerOS/firmware/main/mqtt_manager.cpp`
- `TigerOS/firmware/main/nvs_store.cpp`
- `TigerOS/firmware/main/nvs_store.h`
- `TigerOS/firmware/main/ota_manager.cpp`
- `TigerOS/firmware/main/ota_manager.h`
- `TigerOS/firmware/web/app.js`
- `TigerOS/cloud/src/config.ts`
- `TigerOS/cloud/src/index.ts`
- `TigerOS/cloud/src/routes/devices.ts`
- `TigerOS/cloud/src/routes/ota.ts`
- `TigerOS/REVIEW_REPORT.md`
- `TigerOS/docs/REQUIREMENTS_TRACKER.md`
- `TigerOS/docs/BLE_PROVISIONING_PLAN.md`

## Remaining Risks

- Remote OTA has not been tested against a live HTTPS firmware server in this pass.
- Firmware integration tests on physical ESP32-S3 hardware are still required after flashing.
- Cloud database migrations were not applied to a live PostgreSQL instance during this pass.
- BLE behavior varies across mobile OS versions and should be tested with nRF Connect on iOS and Android.
- MQTT/Home Assistant discovery was not exercised against a real broker/Home Assistant instance in this pass.

## Test Checklist

### Build

- [x] `cd TigerOS/firmware && idf.py build`
- [x] `cd TigerOS/cloud && npm run build`

### Firmware Smoke Test

- [ ] Flash `firmware/build/TigerOS.bin`.
- [ ] Boot with no WiFi credentials and confirm SoftAP `TigerOS-Setup-XXXX`.
- [ ] Log in with default credentials.
- [ ] Scan WiFi networks from Web Console.
- [ ] Save WiFi credentials and confirm LAN IP.
- [ ] Reboot and confirm auto-connect.
- [ ] Upload local OTA and confirm rollback-safe boot.
- [ ] Configure Cloud OTA URL/token/channel and run Check Update.
- [ ] Run Cloud OTA Update Now against a valid HTTPS binary and SHA256.
- [ ] Trigger Factory Reset and confirm WiFi/MQTT/OTA/BLE config are cleared.

### Cloud Smoke Test

- [ ] `docker compose up -d`
- [ ] `npm run prisma:migrate`
- [ ] Register a user.
- [ ] Login and capture JWT.
- [ ] Register a device and capture device token.
- [ ] Attempt to register the same `device_id` from another user and confirm `409`.
- [ ] Create firmware release metadata.
- [ ] Call `/api/ota/check` with device token and confirm update response.
- [ ] Call `/api/devices/heartbeat` and confirm device status update.

### Integration

- [ ] Connect MQTT broker and confirm status/telemetry publish.
- [ ] Confirm Home Assistant MQTT discovery entities appear.
- [ ] Test BLE provisioning with nRF Connect.
- [ ] Confirm logs show OTA, MQTT, BLE, and WiFi lifecycle events.
