#pragma once
#include "device_adapter.h"
namespace tigeros { class GenericAdapter final : public DeviceAdapter { public: const char* name() const override { return "generic"; } }; }
