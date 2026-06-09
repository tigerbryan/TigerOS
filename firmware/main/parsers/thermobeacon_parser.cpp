#include "parsers/thermobeacon_parser.h"

#include <algorithm>
#include <cstdio>

namespace tigeros {

namespace {

constexpr uint16_t THERMOBEACON_SERVICE_UUID = 0xFFF0;

bool has_thermobeacon_service(const BleAdvertisement& adv) {
    for (const auto& field : adv.fields) {
        if (field_is_service_uuid16(field, THERMOBEACON_SERVICE_UUID)) {
            return true;
        }
    }
    return false;
}

bool is_known_thermobeacon_id(uint8_t id) {
    switch (id) {
        case 0x1B:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
        case 0x20:
            return true;
        default:
            return false;
    }
}

int battery_from_voltage_mv(uint16_t voltage_mv) {
    float battery = 0.0f;
    if (voltage_mv >= 3000) {
        battery = 100.0f;
    } else if (voltage_mv >= 2600) {
        battery = 60.0f + (voltage_mv - 2600) * 0.1f;
    } else if (voltage_mv >= 2500) {
        battery = 40.0f + (voltage_mv - 2500) * 0.2f;
    } else if (voltage_mv >= 2450) {
        battery = 20.0f + (voltage_mv - 2450) * 0.4f;
    }
    battery = std::max(0.0f, std::min(100.0f, battery));
    return static_cast<int>(battery + 0.5f);
}

std::string thermobeacon_debug(uint16_t voltage_mv, float temperature_c, float humidity_percent) {
    char buffer[160];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "ThermoBeacon 0xFFF0 advertisement parsed; voltage=%umV temp=%.2fC humidity=%.2f%%",
                  voltage_mv,
                  temperature_c,
                  humidity_percent);
    return std::string(buffer);
}

} // namespace

ParsedBleSensorData parse_thermobeacon_sensor(const BleAdvertisement& adv) {
    ParsedBleSensorData out;
    const bool has_service = has_thermobeacon_service(adv);
    if (!has_service) {
        return out;
    }

    for (const auto& field : adv.fields) {
        if (field.type != 0xFF || field.data.size() != 20 || !is_known_thermobeacon_id(field.data[0])) {
            continue;
        }

        const uint16_t voltage_mv = read_le_u16(field.data.data() + 10);
        const float temperature_c = read_le_i16(field.data.data() + 12) / 16.0f;
        const float humidity_percent = read_le_u16(field.data.data() + 14) / 16.0f;
        if (temperature_c < -50.0f || temperature_c > 100.0f || humidity_percent < 0.0f || humidity_percent > 100.0f) {
            continue;
        }

        out.recognized = true;
        out.brand = "thermobeacon";
        out.model = "ThermoBeacon";
        out.protocol = "thermobeacon";
        out.parse_status = "ok";
        out.has_temperature = true;
        out.has_humidity = true;
        out.has_battery = true;
        out.temperature_c = temperature_c;
        out.humidity_percent = humidity_percent;
        out.battery_percent = battery_from_voltage_mv(voltage_mv);
        out.debug = thermobeacon_debug(voltage_mv, temperature_c, humidity_percent);
        return out;
    }

    out.recognized = true;
    out.brand = "thermobeacon";
    out.model = "ThermoBeacon";
    out.protocol = "ble_raw";
    out.parse_status = "partial";
    out.debug = "ThermoBeacon 0xFFF0 service detected, but this packet is not a 20-byte sensor data frame";
    return out;
}

} // namespace tigeros
