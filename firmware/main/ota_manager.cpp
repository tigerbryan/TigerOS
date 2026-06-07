#include "ota_manager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

#include "ble_sensor_gateway.h"
#include "cJSON.h"
#include "device_manager.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "nvs_store.h"
#include "tiger_log.h"
#include "wifi_manager.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "ota_manager";
constexpr size_t OTA_CHUNK_SIZE = 4096;
constexpr int CHECK_TIMEOUT_MS = 12000;
constexpr int DOWNLOAD_TIMEOUT_MS = 20000;

std::string json_string(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

bool json_bool(cJSON* root, const char* key, bool fallback = false) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

bool is_sha256_hex(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

std::string lowercase_hex(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string to_hex(const uint8_t* digest, size_t len) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = HEX[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = HEX[digest[i] & 0x0f];
    }
    return out;
}

RemoteOtaConfig build_current_config() {
    auto ota = nvs_store().get_ota_config();
    auto status = device_manager().status();
    RemoteOtaConfig config;
    config.enabled = ota.enabled;
    config.auto_update = ota.auto_update;
    config.ota_check_url = ota.check_url;
    config.current_version = status.firmware_version;
    config.device_id = status.device_id;
    config.channel = ota.channel;
    config.hardware_model = status.hardware_model;
    config.flash_size = status.flash_size;
    config.chip_model = status.chip_model;
    return config;
}

void delayed_reboot_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void scheduled_check_task(void* arg) {
    auto* manager = static_cast<OtaManager*>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        while (!wifi_manager().status().connected) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }

        auto config = build_current_config();
        if (config.enabled && !config.ota_check_url.empty()) {
            tiger_log("INFO", TAG, "Scheduled cloud OTA check started");
            auto result = manager->check_remote_update(config);
            if (result.ok && result.update_available && (result.force || config.auto_update)) {
                tiger_log("WARN", TAG, "Scheduled OTA update will install automatically");
                if (manager->install_remote_update(result) == ESP_OK) {
                    xTaskCreate(delayed_reboot_task, "ota_reboot", 2048, nullptr, 5, nullptr);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(24 * 60 * 60 * 1000));
    }
}

} // namespace

esp_err_t OtaManager::init() {
    if (!lock_) {
        lock_ = xSemaphoreCreateMutex();
        if (!lock_) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (scheduler_started_) {
        return ESP_OK;
    }
    xTaskCreate(scheduled_check_task, "ota_scheduler", 8192, this, 4, nullptr);
    scheduler_started_ = true;
    return ESP_OK;
}

esp_err_t OtaManager::handle_upload(httpd_req_t* req) {
    ble_sensor_gateway().pause_auto_scan("local OTA upload");
    if (req->content_len <= 0) {
        tiger_log("ERROR", TAG, "OTA upload rejected: empty body");
        ble_sensor_gateway().resume_auto_scan();
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA update partition found");
        tiger_log("ERROR", TAG, "No OTA update partition found");
        ble_sensor_gateway().resume_auto_scan();
        return ESP_FAIL;
    }

    char start_message[160];
    std::snprintf(start_message, sizeof(start_message), "Local OTA upload started partition=%s size=%d", update_partition->label, req->content_len);
    tiger_log("INFO", TAG, start_message);
    ESP_LOGI(TAG, "Writing OTA image to partition %s, size=%d", update_partition->label, req->content_len);
    esp_ota_handle_t update_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        tiger_log("ERROR", TAG, esp_err_to_name(err));
        ble_sensor_gateway().resume_auto_scan();
        return err;
    }

    std::unique_ptr<char[]> buffer(new char[OTA_CHUNK_SIZE]);
    int remaining = req->content_len;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buffer.get(), std::min<int>(remaining, OTA_CHUNK_SIZE));
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv_len <= 0) {
            esp_ota_abort(update_handle);
            ESP_LOGE(TAG, "OTA upload receive failed");
            tiger_log("ERROR", TAG, "OTA upload receive failed");
            ble_sensor_gateway().resume_auto_scan();
            return ESP_FAIL;
        }

        err = esp_ota_write(update_handle, buffer.get(), recv_len);
        if (err != ESP_OK) {
            esp_ota_abort(update_handle);
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            tiger_log("ERROR", TAG, esp_err_to_name(err));
            ble_sensor_gateway().resume_auto_scan();
            return err;
        }
        remaining -= recv_len;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        tiger_log("ERROR", TAG, esp_err_to_name(err));
        ble_sensor_gateway().resume_auto_scan();
        return err;
    }

    // The new image is selected only after esp_ota_end validates the app image.
    // With rollback enabled, IDF boots it as pending verification.
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        tiger_log("ERROR", TAG, esp_err_to_name(err));
        ble_sensor_gateway().resume_auto_scan();
        return err;
    }

    ESP_LOGI(TAG, "OTA update complete; reboot required");
    tiger_log("INFO", TAG, "OTA update complete; reboot scheduled");
    return ESP_OK;
}

esp_err_t OtaManager::mark_app_valid_after_successful_boot() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) {
        // Factory-flashed images and some boot states do not have an OTA image
        // state. That is not a boot failure; rollback validation only applies
        // to OTA images pending verification.
        tiger_log("WARN", TAG, "OTA image state unavailable; skipping rollback validation");
        return ESP_OK;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        // TigerOS marks the image valid only after NVS, WiFi mode, web server,
        // auth, and logging are initialized. If boot loops before this point,
        // the bootloader can roll back to the previous app.
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            tiger_log("INFO", TAG, "OTA image marked valid after successful boot");
        }
        return err;
    }
    return ESP_OK;
}

RemoteOtaCheckResult OtaManager::check_remote_update(const RemoteOtaConfig& config) {
    RemoteOtaCheckResult result;
    set_last_remote_check(result);
    if (!config.enabled || config.ota_check_url.empty()) {
        result.error = "Cloud OTA is disabled or URL is empty";
        set_last_remote_check(result);
        tiger_log("WARN", TAG, "Cloud OTA check skipped");
        return result;
    }

    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "device_id", config.device_id.c_str());
    cJSON_AddStringToObject(request, "current_version", config.current_version.c_str());
    cJSON_AddStringToObject(request, "hardware_model", config.hardware_model.c_str());
    cJSON_AddStringToObject(request, "channel", config.channel.c_str());
    cJSON_AddNumberToObject(request, "flash_size", config.flash_size);
    cJSON_AddStringToObject(request, "chip_model", config.chip_model.c_str());
    char* body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    std::string response_body;
    esp_http_client_config_t http_config = {};
    http_config.url = config.ota_check_url.c_str();
    http_config.method = HTTP_METHOD_POST;
    http_config.timeout_ms = CHECK_TIMEOUT_MS;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.user_data = &response_body;
    http_config.event_handler = [](esp_http_client_event_t* evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
            auto* response = static_cast<std::string*>(evt->user_data);
            response->append(static_cast<const char*>(evt->data), evt->data_len);
        }
        return ESP_OK;
    };

    tiger_log("INFO", TAG, "Cloud OTA check request started");
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        cJSON_free(body);
        result.error = "Failed to initialize HTTP client";
        set_last_remote_check(result);
        return result;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    const std::string token = nvs_store().get_cloud_token();
    if (!token.empty()) {
        const std::string bearer = "Bearer " + token;
        esp_http_client_set_header(client, "Authorization", bearer.c_str());
    }
    esp_http_client_set_post_field(client, body, std::strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_free(body);

    if (err != ESP_OK || status < 200 || status >= 300) {
        result.error = "OTA check HTTP request failed";
        set_last_remote_check(result);
        tiger_log("ERROR", TAG, "Cloud OTA check failed");
        return result;
    }

    cJSON* root = cJSON_Parse(response_body.c_str());
    if (!root) {
        result.error = "Invalid OTA check JSON response";
        set_last_remote_check(result);
        tiger_log("ERROR", TAG, "Cloud OTA check returned invalid JSON");
        return result;
    }
    result.ok = true;
    result.update_available = json_bool(root, "update_available");
    result.force = json_bool(root, "force");
    result.version = json_string(root, "version");
    result.firmware_url = json_string(root, "firmware_url");
    result.sha256 = lowercase_hex(json_string(root, "sha256"));
    result.release_notes = json_string(root, "release_notes");
    cJSON_Delete(root);

    if (result.update_available && (result.firmware_url.empty() || !is_sha256_hex(result.sha256))) {
        result.ok = false;
        result.error = "OTA response missing firmware_url or valid sha256";
    }
    set_last_remote_check(result);
    tiger_log("INFO", TAG, result.update_available ? "Cloud OTA update available" : "Cloud OTA is up to date");
    return result;
}

esp_err_t OtaManager::install_remote_update(const RemoteOtaCheckResult& update) {
    if (!update.update_available || update.firmware_url.empty()) {
        tiger_log("WARN", TAG, "Remote OTA install skipped: no update URL");
        return ESP_ERR_INVALID_ARG;
    }
    const std::string expected_sha = lowercase_hex(update.sha256);
    if (!is_sha256_hex(expected_sha)) {
        tiger_log("ERROR", TAG, "Remote OTA install rejected: missing SHA256");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        tiger_log("ERROR", TAG, "Remote OTA failed: no update partition");
        return ESP_FAIL;
    }

    esp_http_client_config_t http_config = {};
    http_config.url = update.firmware_url.c_str();
    http_config.method = HTTP_METHOD_GET;
    http_config.timeout_ms = DOWNLOAD_TIMEOUT_MS;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        return ESP_FAIL;
    }

    set_remote_progress(0);
    tiger_log("INFO", TAG, "Remote OTA firmware download started");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        tiger_log("ERROR", TAG, "Remote OTA HTTP open failed");
        return err;
    }

    const int content_length = esp_http_client_fetch_headers(client);
    const int http_status = esp_http_client_get_status_code(client);
    if (http_status < 200 || http_status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        tiger_log("ERROR", TAG, "Remote OTA download HTTP status failed");
        return ESP_FAIL;
    }
    if (content_length > static_cast<int>(update_partition->size)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        tiger_log("ERROR", TAG, "Remote OTA image size invalid for partition");
        return ESP_ERR_INVALID_SIZE;
    }
    if (content_length <= 0) {
        tiger_log("WARN", TAG, "Remote OTA download has no Content-Length; SHA256 will be the final validation");
    }
    esp_ota_handle_t update_handle = 0;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        tiger_log("ERROR", TAG, "Remote OTA esp_ota_begin failed");
        return err;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    std::unique_ptr<char[]> buffer(new char[OTA_CHUNK_SIZE]);
    int downloaded = 0;
    while (true) {
        int read_len = esp_http_client_read(client, buffer.get(), OTA_CHUNK_SIZE);
        if (read_len < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        err = esp_ota_write(update_handle, buffer.get(), read_len);
        if (err != ESP_OK) {
            break;
        }
        mbedtls_sha256_update(&sha_ctx, reinterpret_cast<const unsigned char*>(buffer.get()), read_len);
        downloaded += read_len;
        if (content_length > 0) {
            set_remote_progress(static_cast<uint8_t>(std::min(99, (downloaded * 100) / content_length)));
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);

    if (err != ESP_OK) {
        esp_ota_abort(update_handle);
        set_remote_progress(0);
        tiger_log("ERROR", TAG, "Remote OTA download/write failed");
        return err;
    }
    const std::string actual_sha = to_hex(digest, sizeof(digest));
    if (content_length > 0 && downloaded != content_length) {
        esp_ota_abort(update_handle);
        set_remote_progress(0);
        tiger_log("ERROR", TAG, "Remote OTA download length mismatch");
        return ESP_ERR_INVALID_SIZE;
    }
    if (actual_sha != expected_sha) {
        esp_ota_abort(update_handle);
        set_remote_progress(0);
        tiger_log("ERROR", TAG, "Remote OTA SHA256 verification failed");
        return ESP_ERR_INVALID_CRC;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        set_remote_progress(0);
        tiger_log("ERROR", TAG, "Remote OTA image validation failed");
        return err;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        set_remote_progress(0);
        tiger_log("ERROR", TAG, "Remote OTA boot partition update failed");
        return err;
    }

    set_remote_progress(100);
    tiger_log("INFO", TAG, "Remote OTA installed successfully");
    return ESP_OK;
}

RemoteOtaCheckResult OtaManager::last_remote_check() {
    if (lock_) xSemaphoreTake(lock_, portMAX_DELAY);
    RemoteOtaCheckResult result = last_check_;
    if (lock_) xSemaphoreGive(lock_);
    return result;
}

uint8_t OtaManager::remote_progress() {
    if (lock_) xSemaphoreTake(lock_, portMAX_DELAY);
    uint8_t progress = remote_progress_;
    if (lock_) xSemaphoreGive(lock_);
    return progress;
}

void OtaManager::set_last_remote_check(const RemoteOtaCheckResult& result) {
    if (lock_) xSemaphoreTake(lock_, portMAX_DELAY);
    last_check_ = result;
    if (lock_) xSemaphoreGive(lock_);
}

void OtaManager::set_remote_progress(uint8_t progress) {
    if (lock_) xSemaphoreTake(lock_, portMAX_DELAY);
    remote_progress_ = progress;
    if (lock_) xSemaphoreGive(lock_);
}

OtaManager& ota_manager() {
    static OtaManager manager;
    return manager;
}

} // namespace tigeros
