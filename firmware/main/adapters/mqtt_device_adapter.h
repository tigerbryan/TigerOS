#pragma once
#include "device_adapter.h"
namespace tigeros { class MqttDeviceAdapter final : public DeviceAdapter { public: const char* name() const override { return "mqtt_device"; } }; }
