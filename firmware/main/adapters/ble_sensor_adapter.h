#pragma once
#include "device_adapter.h"
namespace tigeros { class BleSensorAdapter final : public DeviceAdapter { public: const char* name() const override { return "ble_sensor"; } bool enabled() const override { return true; } }; }
