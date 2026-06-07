#include "parsers/atc_parser.h"

namespace tigeros {

ParsedBleSensorData parse_atc_sensor(const BleAdvertisement& adv) {
    ParsedBleSensorData out;
    const bool likely_name = adv.name.rfind("ATC_", 0) == 0 || adv.name.rfind("PVVX", 0) == 0;

    for (const auto& field : adv.fields) {
        if (!field_is_service_data_uuid16(field, 0x181A)) {
            continue;
        }
        const uint8_t* payload = field.data.data() + 2;
        const size_t len = field.data.size() - 2;

        // PVVX custom format: MAC(6), temp int16 LE / 100,
        // humidity u16 LE / 100, battery_mv u16 LE, battery u8,
        // counter u8, flags u8.
        if (len >= 14) {
            const float temp = read_le_i16(payload + 6) / 100.0f;
            const float humidity = read_le_u16(payload + 8) / 100.0f;
            const int battery = payload[12];
            if (temp > -50.0f && temp < 100.0f && humidity >= 0.0f && humidity <= 100.0f && battery >= 0 && battery <= 100) {
                out.recognized = true;
                out.brand = "xiaomi";
                out.model = "LYWSD03MMC";
                out.protocol = "pvvx";
                out.parse_status = "ok";
                out.has_temperature = true;
                out.has_humidity = true;
                out.has_battery = true;
                out.temperature_c = temp;
                out.humidity_percent = humidity;
                out.battery_percent = battery;
                return out;
            }
        }

        // ATC custom format: MAC(6), temp int16 BE / 10, humidity u8,
        // battery u8, battery_mv u16 BE, counter u8.
        if (len >= 11) {
            const float temp = read_be_i16(payload + 6) / 10.0f;
            const int humidity = payload[8];
            const int battery = payload[9];
            if (temp > -50.0f && temp < 100.0f && humidity >= 0 && humidity <= 100 && battery >= 0 && battery <= 100) {
                out.recognized = true;
                out.brand = "xiaomi";
                out.model = "LYWSD03MMC";
                out.protocol = "atc";
                out.parse_status = "ok";
                out.has_temperature = true;
                out.has_humidity = true;
                out.has_battery = true;
                out.temperature_c = temp;
                out.humidity_percent = humidity;
                out.battery_percent = battery;
                return out;
            }
        }
    }

    if (likely_name) {
        out.recognized = true;
        out.brand = "xiaomi";
        out.model = "LYWSD03MMC";
        out.protocol = "atc";
        out.parse_status = "partial";
        out.debug = "ATC/PVVX name detected; service data layout needs raw packet confirmation";
    }
    return out;
}

} // namespace tigeros
