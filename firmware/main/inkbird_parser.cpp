#include "inkbird_parser.h"

#include <algorithm>
#include <cstdio>

namespace tigeros {
namespace {

uint16_t read_le_u16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

int16_t read_le_i16(const uint8_t* data) {
    return static_cast<int16_t>(read_le_u16(data));
}

} // namespace

std::string bytes_to_hex(const uint8_t* data, size_t len) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = HEX[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX[data[i] & 0x0F];
    }
    return out;
}

std::string bytes_to_hex(const std::vector<uint8_t>& data) {
    return bytes_to_hex(data.data(), data.size());
}

InkbirdParseResult parse_inkbird_advertisement(const std::string& name, const std::vector<uint8_t>& advertisement) {
    InkbirdParseResult result;
    const bool likely_inkbird_name = name == "sps" || name == "SPS";

    size_t index = 0;
    while (index < advertisement.size()) {
        const uint8_t field_len = advertisement[index];
        if (field_len == 0) {
            break;
        }
        const size_t field_start = index + 1;
        const size_t field_end = field_start + field_len;
        if (field_end > advertisement.size() || field_len < 1) {
            result.debug = "Malformed BLE AD structure";
            return result;
        }

        const uint8_t type = advertisement[field_start];
        const uint8_t* payload = advertisement.data() + field_start + 1;
        const size_t payload_len = field_len - 1;

        if (type == 0xFF && payload_len >= 9) {
            // ESPHome's Inkbird IBS-TH1/TH2 reference treats the first two
            // manufacturer bytes as signed temperature * 100 and the next
            // seven bytes as humidity/battery/status. Some model variants may
            // differ, so unknown-looking packets still remain visible as raw.
            const int16_t temp_raw = read_le_i16(payload);
            const uint8_t* data = payload + 2;
            const size_t data_len = payload_len - 2;
            if (data_len >= 7 && (data[6] == 8 || data[6] == 6) && (data[2] == 0 || data[2] == 1)) {
                const float measured_temperature = temp_raw / 100.0f;
                const float humidity = read_le_u16(data) / 100.0f;
                const int battery = data[5];
                if (humidity >= 0.0f && humidity <= 100.0f && battery >= 0 && battery <= 100) {
                    result.recognized = true;
                    result.sensor_type = "inkbird_ibsth2";
                    if (data[2] == 0) {
                        result.has_temperature = true;
                        result.temperature_c = measured_temperature;
                    } else {
                        result.has_external_probe_temperature = true;
                        result.external_probe_temperature_c = measured_temperature;
                    }
                    result.has_humidity = true;
                    result.has_battery = true;
                    result.humidity_percent = humidity;
                    result.battery_percent = battery;
                    return result;
                }
            }
        }

        index = field_end;
    }

    if (likely_inkbird_name) {
        result.debug = "Name matches Inkbird-style 'sps', but packet format is not recognized";
    } else {
        result.debug = "No supported Inkbird manufacturer data found";
    }
    return result;
}

} // namespace tigeros
