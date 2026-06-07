#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string>

namespace tigeros {

struct RemoteOtaConfig {
    bool enabled = false;
    bool auto_update = false;
    std::string ota_check_url;
    std::string current_version;
    std::string device_id;
    std::string channel;
    std::string hardware_model;
    uint32_t flash_size = 0;
    std::string chip_model;
};

struct RemoteOtaCheckResult {
    bool ok = false;
    bool update_available = false;
    bool force = false;
    std::string version;
    std::string firmware_url;
    std::string sha256;
    std::string release_notes;
    std::string error;
};

class OtaManager {
public:
    esp_err_t init();
    esp_err_t handle_upload(httpd_req_t* req);
    esp_err_t mark_app_valid_after_successful_boot();
    RemoteOtaCheckResult check_remote_update(const RemoteOtaConfig& config);
    esp_err_t install_remote_update(const RemoteOtaCheckResult& update);
    RemoteOtaCheckResult last_remote_check();
    uint8_t remote_progress();

private:
    void set_last_remote_check(const RemoteOtaCheckResult& result);
    void set_remote_progress(uint8_t progress);

    bool scheduler_started_ = false;
    SemaphoreHandle_t lock_ = nullptr;
    RemoteOtaCheckResult last_check_;
    uint8_t remote_progress_ = 0;
};

OtaManager& ota_manager();

} // namespace tigeros
