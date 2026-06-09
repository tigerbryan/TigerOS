#include "ble_sensor_registry.h"

#include "parsers/atc_parser.h"
#include "parsers/bthome_parser.h"
#include "parsers/inkbird_parser.h"
#include "parsers/thermobeacon_parser.h"
#include "parsers/unknown_ble_parser.h"
#include "parsers/xiaomi_parser.h"

namespace tigeros {

ParsedBleSensorData parse_ble_sensor_advertisement(const BleAdvertisement& adv) {
    ParsedBleSensorData parsed = parse_bthome_sensor(adv);
    if (parsed.recognized && parsed.parse_status == "ok") return parsed;

    ParsedBleSensorData atc = parse_atc_sensor(adv);
    if (atc.recognized && atc.parse_status == "ok") return atc;
    if (!parsed.recognized && atc.recognized) parsed = atc;

    ParsedBleSensorData inkbird = parse_inkbird_sensor(adv);
    if (inkbird.recognized && inkbird.parse_status == "ok") return inkbird;
    if (!parsed.recognized && inkbird.recognized) parsed = inkbird;

    ParsedBleSensorData thermobeacon = parse_thermobeacon_sensor(adv);
    if (thermobeacon.recognized && thermobeacon.parse_status == "ok") return thermobeacon;
    if (!parsed.recognized && thermobeacon.recognized) parsed = thermobeacon;

    ParsedBleSensorData xiaomi = parse_xiaomi_sensor(adv);
    if (xiaomi.recognized && (xiaomi.parse_status == "encrypted" || xiaomi.parse_status == "partial")) return xiaomi;
    if (!parsed.recognized && xiaomi.recognized) parsed = xiaomi;

    if (!parsed.recognized) {
        parsed = parse_unknown_ble_sensor(adv);
    }
    parsed.mac_address = adv.mac_address;
    parsed.name = adv.name;
    parsed.rssi = adv.rssi;
    parsed.last_seen = adv.last_seen;
    parsed.raw_adv_hex = bytes_to_hex(adv.raw);
    return parsed;
}

} // namespace tigeros
