#pragma once

#include <string>

namespace tigeros {

struct DeviceStatus {
    std::string device_id;
    std::string firmware_version;
    std::string build_time;
    std::string wifi_mode;
    std::string wifi_ssid;
    std::string ap_ssid;
    std::string ip_address;
    std::string hardware_model = "esp32-s3";
    std::string chip_model;
    bool wifi_connected = false;
    uint64_t uptime_seconds = 0;
    uint32_t free_heap = 0;
    uint32_t minimum_free_heap = 0;
    uint32_t flash_size = 0;
};

class DeviceManager {
public:
    void init(const std::string& firmware_version, const std::string& build_time);
    DeviceStatus status();

private:
    std::string firmware_version_;
    std::string build_time_;
};

DeviceManager& device_manager();

} // namespace tigeros
