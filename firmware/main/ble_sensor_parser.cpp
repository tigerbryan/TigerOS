#include "ble_sensor_parser.h"

namespace tigeros {

std::vector<BleAdField> parse_ble_ad_fields(const uint8_t* data, size_t len) {
    std::vector<BleAdField> fields;
    size_t index = 0;
    while (index < len) {
        const uint8_t field_len = data[index];
        if (field_len == 0) {
            break;
        }
        const size_t field_start = index + 1;
        const size_t field_end = field_start + field_len;
        if (field_end > len || field_len < 1) {
            break;
        }
        BleAdField field;
        field.type = data[field_start];
        field.data.assign(data + field_start + 1, data + field_end);
        fields.push_back(field);
        index = field_end;
    }
    return fields;
}

std::string ble_name_from_fields(const std::vector<BleAdField>& fields) {
    for (const auto& field : fields) {
        if ((field.type == 0x08 || field.type == 0x09) && !field.data.empty()) {
            return std::string(reinterpret_cast<const char*>(field.data.data()), field.data.size());
        }
    }
    return {};
}

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

uint16_t read_le_u16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

int16_t read_le_i16(const uint8_t* data) {
    return static_cast<int16_t>(read_le_u16(data));
}

uint16_t read_be_u16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]);
}

int16_t read_be_i16(const uint8_t* data) {
    return static_cast<int16_t>(read_be_u16(data));
}

bool field_is_service_data_uuid16(const BleAdField& field, uint16_t uuid) {
    return field.type == 0x16 && field.data.size() >= 2 && read_le_u16(field.data.data()) == uuid;
}

bool field_is_service_uuid16(const BleAdField& field, uint16_t uuid) {
    if ((field.type != 0x02 && field.type != 0x03) || field.data.size() < 2) {
        return false;
    }
    for (size_t i = 0; i + 1 < field.data.size(); i += 2) {
        if (read_le_u16(field.data.data() + i) == uuid) {
            return true;
        }
    }
    return false;
}

} // namespace tigeros
