#include "parsers/unknown_ble_parser.h"

namespace tigeros {

ParsedBleSensorData parse_unknown_ble_sensor(const BleAdvertisement&) {
    ParsedBleSensorData out;
    out.recognized = false;
    out.brand = "unknown";
    out.model = "unknown";
    out.protocol = "unknown";
    out.parse_status = "unknown";
    out.debug = "No supported parser matched; raw packet retained for debugging";
    return out;
}

} // namespace tigeros
