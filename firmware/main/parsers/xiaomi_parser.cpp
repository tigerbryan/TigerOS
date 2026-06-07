#include "parsers/xiaomi_parser.h"

namespace tigeros {

ParsedBleSensorData parse_xiaomi_sensor(const BleAdvertisement& adv) {
    ParsedBleSensorData out;
    const bool likely_name = adv.name.find("LYWSD03MMC") != std::string::npos ||
                             adv.name.rfind("MHO", 0) == 0 ||
                             adv.name.find("Xiaomi") != std::string::npos;

    for (const auto& field : adv.fields) {
        if (field_is_service_data_uuid16(field, 0xFE95)) {
            out.recognized = true;
            out.brand = "xiaomi";
            out.model = adv.name.find("LYWSD03MMC") != std::string::npos ? "LYWSD03MMC" : "unknown";
            out.protocol = "xiaomi";
            out.parse_status = "encrypted";
            out.debug = "Xiaomi stock encrypted frame detected; bindkey support is a placeholder";
            return out;
        }
    }

    if (likely_name) {
        out.recognized = true;
        out.brand = "xiaomi";
        out.model = adv.name.find("LYWSD03MMC") != std::string::npos ? "LYWSD03MMC" : "unknown";
        out.protocol = "xiaomi";
        out.parse_status = "partial";
        out.debug = "Xiaomi-style name detected; stock protocol parser awaits real raw packets";
    }
    return out;
}

} // namespace tigeros
