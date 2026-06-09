#include "parsers/inkbird_parser.h"

namespace tigeros {

ParsedBleSensorData parse_inkbird_sensor(const BleAdvertisement& adv) {
    ParsedBleSensorData out;
    const bool likely_name = adv.name == "sps" || adv.name == "SPS";
    bool has_inkbird_candidate_uuid = false;

    for (const auto& field : adv.fields) {
        if (field_is_service_uuid16(field, 0xFFF0)) {
            has_inkbird_candidate_uuid = true;
            continue;
        }
        if (field.type != 0xFF || field.data.size() < 9) {
            continue;
        }
        const int16_t temp_raw = read_le_i16(field.data.data());
        const uint8_t* data = field.data.data() + 2;
        const size_t data_len = field.data.size() - 2;
        if (data_len < 7 || (data[6] != 8 && data[6] != 6) || (data[2] != 0 && data[2] != 1)) {
            continue;
        }
        const float humidity = read_le_u16(data) / 100.0f;
        const int battery = data[5];
        if (humidity < 0.0f || humidity > 100.0f || battery < 0 || battery > 100) {
            continue;
        }

        out.recognized = true;
        out.brand = "inkbird";
        out.model = data[6] == 8 ? "IBS-TH2" : "IBS-TH2 Plus";
        out.protocol = "inkbird";
        out.parse_status = "ok";
        out.has_humidity = true;
        out.has_battery = true;
        out.humidity_percent = humidity;
        out.battery_percent = battery;
        const float temperature = temp_raw / 100.0f;
        if (data[2] == 0) {
            out.has_temperature = true;
            out.temperature_c = temperature;
        } else {
            out.has_external_probe_temperature = true;
            out.external_probe_temperature_c = temperature;
        }
        return out;
    }

    if (likely_name) {
        out.recognized = true;
        out.brand = "inkbird";
        out.model = "IBS-TH2";
        out.protocol = "inkbird";
        out.parse_status = "partial";
        out.debug = "Inkbird-style name detected but manufacturer payload did not match known layout";
    } else if (has_inkbird_candidate_uuid) {
        // 0xFFF0 is used by several BLE thermometers and is not unique to
        // Inkbird. Keep the packet visible for diagnostics, but do not label
        // the device as Inkbird until a known Inkbird payload is present.
        out.recognized = true;
        out.brand = "generic";
        out.model = "BLE 0xFFF0 sensor";
        out.protocol = "ble_raw";
        out.parse_status = "partial";
        out.debug = "BLE service UUID 0xFFF0 detected, but manufacturer payload is not a supported Inkbird format";
    }
    return out;
}

} // namespace tigeros
