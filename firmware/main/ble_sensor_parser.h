#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tigeros {

struct BleAdField {
    uint8_t type = 0;
    std::vector<uint8_t> data;
};

struct BleAdvertisement {
    std::string mac_address;
    std::string name;
    int rssi = 0;
    uint64_t last_seen = 0;
    std::vector<uint8_t> raw;
    std::vector<BleAdField> fields;
};

struct ParsedBleSensorData {
    bool recognized = false;
    std::string mac_address;
    std::string name;
    std::string brand = "unknown";
    std::string model = "unknown";
    std::string protocol = "unknown";
    std::string parse_status = "unknown";
    std::string debug;
    int rssi = 0;
    uint64_t last_seen = 0;
    std::string raw_adv_hex;
    bool has_temperature = false;
    bool has_humidity = false;
    bool has_battery = false;
    bool has_external_probe_temperature = false;
    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
    int battery_percent = 0;
    float external_probe_temperature_c = 0.0f;
};

std::vector<BleAdField> parse_ble_ad_fields(const uint8_t* data, size_t len);
std::string ble_name_from_fields(const std::vector<BleAdField>& fields);
std::string bytes_to_hex(const uint8_t* data, size_t len);
std::string bytes_to_hex(const std::vector<uint8_t>& data);
uint16_t read_le_u16(const uint8_t* data);
int16_t read_le_i16(const uint8_t* data);
uint16_t read_be_u16(const uint8_t* data);
int16_t read_be_i16(const uint8_t* data);
bool field_is_service_data_uuid16(const BleAdField& field, uint16_t uuid);
bool field_is_service_uuid16(const BleAdField& field, uint16_t uuid);

} // namespace tigeros
