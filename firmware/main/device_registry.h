#pragma once

#include <optional>
#include <string>
#include <vector>

#include "device_adapter.h"
#include "esp_err.h"

struct cJSON;

namespace tigeros {

class DeviceRegistry {
public:
    esp_err_t init();
    std::vector<UniversalDevice> devices();
    std::vector<UniversalDevice> discovered();
    std::optional<UniversalDevice> get(const std::string& id);
    esp_err_t scan(const std::string& method);
    esp_err_t pair(cJSON* payload);
    esp_err_t remove(const std::string& id);
    esp_err_t rename(const std::string& id, const std::string& name);
    esp_err_t set_location(const std::string& id, const std::string& location);
    esp_err_t control(const std::string& id, const std::string& capability, cJSON* value);
    esp_err_t publish_state(const UniversalDevice& device);
};

DeviceRegistry& device_registry();
std::string universal_device_id_from_address(const std::string& protocol, const std::string& address);
cJSON* universal_device_to_json(const UniversalDevice& device, bool include_raw);

} // namespace tigeros
