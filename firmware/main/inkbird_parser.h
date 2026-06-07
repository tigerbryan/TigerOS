#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tigeros {

struct InkbirdParseResult {
    bool recognized = false;
    bool has_temperature = false;
    bool has_humidity = false;
    bool has_battery = false;
    bool has_external_probe_temperature = false;
    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
    int battery_percent = 0;
    float external_probe_temperature_c = 0.0f;
    std::string sensor_type = "unknown_ble_sensor";
    std::string debug;
};

InkbirdParseResult parse_inkbird_advertisement(const std::string& name, const std::vector<uint8_t>& advertisement);
std::string bytes_to_hex(const uint8_t* data, size_t len);
std::string bytes_to_hex(const std::vector<uint8_t>& data);

} // namespace tigeros
