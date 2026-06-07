#pragma once

#include "ble_sensor_parser.h"

namespace tigeros {

ParsedBleSensorData parse_ble_sensor_advertisement(const BleAdvertisement& adv);

} // namespace tigeros
