#pragma once
#include "device_adapter.h"
namespace tigeros { class ShellyAdapter final : public DeviceAdapter { public: const char* name() const override { return "shelly"; } }; }
