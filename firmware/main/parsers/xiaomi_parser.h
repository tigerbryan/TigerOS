#pragma once

#include "ble_sensor_parser.h"

namespace tigeros {

ParsedBleSensorData parse_xiaomi_sensor(const BleAdvertisement& adv);

} // namespace tigeros
