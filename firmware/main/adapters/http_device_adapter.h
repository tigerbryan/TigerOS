#pragma once
#include "device_adapter.h"
namespace tigeros { class HttpDeviceAdapter final : public DeviceAdapter { public: const char* name() const override { return "http_device"; } }; }
