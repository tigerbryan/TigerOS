#pragma once
#include "device_adapter.h"
namespace tigeros { class EsphomeAdapter final : public DeviceAdapter { public: const char* name() const override { return "esphome"; } }; }
