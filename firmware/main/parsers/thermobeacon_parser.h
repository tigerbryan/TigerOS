#pragma once

#include "ble_sensor_parser.h"

namespace tigeros {

ParsedBleSensorData parse_thermobeacon_sensor(const BleAdvertisement& adv);

} // namespace tigeros
