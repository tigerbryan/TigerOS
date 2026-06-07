#include "parsers/bthome_parser.h"

namespace tigeros {
namespace {

int object_payload_size(uint8_t object_id) {
    switch (object_id) {
        case 0x00: // packet id
        case 0x01: // battery
        case 0x09: // count 8-bit
        case 0x0F: // generic boolean
        case 0x10: // power
        case 0x11: // opening
        case 0x12: // co2 alarm
        case 0x13: // carbon monoxide
        case 0x14: // cold
        case 0x15: // connectivity
        case 0x16: // door
        case 0x17: // garage door
        case 0x18: // gas
        case 0x19: // heat
        case 0x1A: // light
        case 0x1B: // lock
        case 0x1C: // moisture
        case 0x1D: // motion
        case 0x1E: // moving
        case 0x1F: // occupancy
        case 0x20: // plug
        case 0x21: // presence
        case 0x22: // problem
        case 0x23: // running
        case 0x24: // safety
        case 0x25: // smoke
        case 0x26: // sound
        case 0x27: // tamper
        case 0x28: // vibration
        case 0x29: // window
        case 0x2D: // window/door style boolean in some emitters
        case 0x2E: // humidity 8-bit
        case 0x2F: // moisture 8-bit
        case 0x3A: // button event
        case 0x46: // UV index
            return 1;
        case 0x02: // temperature 0.01 C
        case 0x03: // humidity 0.01 %
        case 0x06: // mass kg
        case 0x07: // mass lb
        case 0x08: // dewpoint
        case 0x0C: // voltage
        case 0x0D: // pm2.5
        case 0x0E: // pm10
        case 0x3D: // count 16-bit
        case 0x3F: // rotation
        case 0x40: // distance mm
        case 0x41: // distance m
        case 0x43: // current
        case 0x44: // speed
        case 0x45: // temperature 0.1 C
        case 0x47: // volume
        case 0x48: // volume flow rate
        case 0x49: // voltage 0.1 V
            return 2;
        case 0x04: // pressure
        case 0x05: // illuminance
        case 0x0A: // energy
        case 0x0B: // power
        case 0x42: // duration
        case 0x4A: // gas
            return 3;
        case 0x3E: // count 32-bit
            return 4;
        default:
            return -1;
    }
}

bool skip_known_object(const uint8_t* payload, size_t len, size_t& i, uint8_t object_id) {
    const int size = object_payload_size(object_id);
    if (size < 0 || i + static_cast<size_t>(size) > len) {
        return false;
    }
    i += static_cast<size_t>(size);
    return true;
}

} // namespace

ParsedBleSensorData parse_bthome_sensor(const BleAdvertisement& adv) {
    ParsedBleSensorData out;
    for (const auto& field : adv.fields) {
        if (!field_is_service_data_uuid16(field, 0xFCD2) || field.data.size() < 3) {
            continue;
        }
        const uint8_t* payload = field.data.data() + 2;
        const size_t len = field.data.size() - 2;
        const uint8_t info = payload[0];
        if (info & 0x01) {
            out.recognized = true;
            out.brand = "generic";
            out.model = "BTHome";
            out.protocol = "bthome";
            out.parse_status = "encrypted";
            out.debug = "Encrypted BTHome frame detected; bindkey/decryption not implemented yet";
            return out;
        }

        out.recognized = true;
        out.brand = adv.name.find("LYWSD") != std::string::npos || adv.name.rfind("ATC_", 0) == 0 ? "xiaomi" : "generic";
        out.model = out.brand == "xiaomi" ? "LYWSD03MMC" : "BTHome";
        out.protocol = "bthome";
        out.parse_status = "partial";

        size_t i = 1;
        int parsed_objects = 0;
        while (i < len) {
            const uint8_t object_id = payload[i++];
            switch (object_id) {
                case 0x00:
                    if (!skip_known_object(payload, len, i, object_id)) i = len;
                    break;
                case 0x01:
                    if (i + 1 <= len) {
                        out.has_battery = true;
                        out.battery_percent = payload[i++];
                        parsed_objects++;
                    } else {
                        i = len;
                    }
                    break;
                case 0x02:
                    if (i + 2 <= len) {
                        out.has_temperature = true;
                        out.temperature_c = read_le_i16(payload + i) / 100.0f;
                        i += 2;
                        parsed_objects++;
                    } else {
                        i = len;
                    }
                    break;
                case 0x03:
                    if (i + 2 <= len) {
                        out.has_humidity = true;
                        out.humidity_percent = read_le_u16(payload + i) / 100.0f;
                        i += 2;
                        parsed_objects++;
                    } else {
                        i = len;
                    }
                    break;
                case 0x2E:
                    if (i + 1 <= len) {
                        out.has_humidity = true;
                        out.humidity_percent = payload[i++];
                        parsed_objects++;
                    } else {
                        i = len;
                    }
                    break;
                case 0x45:
                    if (i + 2 <= len) {
                        out.has_temperature = true;
                        out.temperature_c = read_le_i16(payload + i) / 10.0f;
                        i += 2;
                        parsed_objects++;
                    } else {
                        i = len;
                    }
                    break;
                default:
                    // BTHome packets often contain objects that TigerOS does
                    // not expose yet. Skip known fixed-length objects so later
                    // temperature/humidity/battery fields can still be parsed.
                    if (!skip_known_object(payload, len, i, object_id)) {
                        out.debug = "BTHome frame contains an unsupported object id; raw hex is available";
                        i = len;
                    }
                    break;
            }
        }
        if (out.has_temperature || out.has_humidity || out.has_battery) {
            out.parse_status = "ok";
            out.debug = "BTHome frame parsed with readable sensor values";
        } else if (parsed_objects > 0) {
            out.parse_status = "partial";
            out.debug = "BTHome frame parsed but did not contain temperature, humidity, or battery";
        }
        return out;
    }
    return out;
}

} // namespace tigeros
