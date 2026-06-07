#pragma once
#include "device_adapter.h"
namespace tigeros { class TasmotaAdapter final : public DeviceAdapter { public: const char* name() const override { return "tasmota"; } }; }
