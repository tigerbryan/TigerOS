#include "device_manager.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_store.h"
#include "wifi_manager.h"

namespace tigeros {

void DeviceManager::init(const std::string& firmware_version, const std::string& build_time) {
    firmware_version_ = firmware_version;
    build_time_ = build_time;
    nvs_store().get_or_create_device_id();
    nvs_store().ensure_firmware_version(firmware_version);
}

DeviceStatus DeviceManager::status() {
    auto wifi = wifi_manager().status();

    DeviceStatus out;
    out.device_id = nvs_store().get_or_create_device_id();
    out.firmware_version = firmware_version_.empty() ? nvs_store().get_firmware_version() : firmware_version_;
    out.build_time = build_time_;
    out.wifi_connected = wifi.connected;
    out.wifi_ssid = wifi.ssid;
    out.ap_ssid = wifi.ap_ssid;
    out.ip_address = wifi.ip_address;
    out.wifi_disconnect_count = wifi.disconnect_count;
    out.wifi_last_disconnect_reason = wifi.last_disconnect_reason;
    out.wifi_mode = wifi.connected ? "station" : (wifi.ap_active ? "setup_ap" : "disconnected");
    out.hardware_model = "esp32-s3";
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    out.chip_model = chip_info.model == CHIP_ESP32S3 ? "ESP32-S3" : "ESP32";
    esp_flash_get_size(nullptr, &out.flash_size);
    out.uptime_seconds = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    out.free_heap = esp_get_free_heap_size();
    out.minimum_free_heap = esp_get_minimum_free_heap_size();
    return out;
}

DeviceManager& device_manager() {
    static DeviceManager manager;
    return manager;
}

} // namespace tigeros
