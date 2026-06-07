#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

struct cJSON;

namespace tigeros {

struct UniversalDevice {
    std::string id;
    std::string name;
    std::string type = "unknown";
    std::string brand = "generic";
    std::string model = "unknown";
    std::string protocol = "unknown";
    std::string adapter = "generic";
    std::string address;
    std::string location;
    bool online = false;
    uint64_t last_seen = 0;
    std::vector<std::string> capabilities;
    std::string state_json = "{}";
    std::string raw_json = "{}";
};

class DeviceAdapter {
public:
    virtual ~DeviceAdapter() = default;
    virtual const char* name() const = 0;
    virtual bool enabled() const { return false; }
    virtual esp_err_t scan() { return ESP_ERR_NOT_SUPPORTED; }
    virtual esp_err_t pair(cJSON*) { return ESP_ERR_NOT_SUPPORTED; }
    virtual esp_err_t remove(const std::string&) { return ESP_ERR_NOT_SUPPORTED; }
    virtual esp_err_t poll() { return ESP_ERR_NOT_SUPPORTED; }
    virtual esp_err_t parse(cJSON*) { return ESP_ERR_NOT_SUPPORTED; }
    virtual std::vector<UniversalDevice> get_state() { return {}; }
    virtual esp_err_t set_state(const std::string&, const std::string&, cJSON*) { return ESP_ERR_NOT_SUPPORTED; }
    virtual esp_err_t publish() { return ESP_ERR_NOT_SUPPORTED; }
    virtual std::vector<std::string> get_capabilities(const std::string&) { return {}; }
};

} // namespace tigeros
