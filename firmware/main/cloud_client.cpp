#include "cloud_client.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "ble_sensor_gateway.h"
#include "cJSON.h"
#include "device_manager.h"
#include "device_registry.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tiger_log.h"
#include "wifi_manager.h"

#include <ctime>
#include <sys/time.h>

namespace tigeros {
namespace {
constexpr const char* TAG = "cloud_client";
constexpr uint32_t CLOUD_TASK_DELAY_MS = 1000;
constexpr int HTTP_TIMEOUT_MS = 6000;
constexpr uint64_t HEARTBEAT_INTERVAL_SECONDS = 60;

struct WebhookHttpTrace {
    int mbedtls_error = 0;
    int tls_flags = 0;
    esp_err_t tls_capture_err = ESP_OK;
    char response[768] = {};
    size_t response_len = 0;
};

uint64_t uptime_seconds();

bool starts_with(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string heartbeat_url_from_webhook_url(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    const size_t path = url.find('/', scheme + 3);
    const std::string origin = path == std::string::npos ? url : url.substr(0, path);
    if (origin.empty()) {
        return {};
    }
    return origin + "/api/public/tigeros/heartbeat";
}

bool system_time_valid() {
    std::time_t now = 0;
    std::time(&now);
    tm timeinfo = {};
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year + 1900 >= 2024;
}

int build_month_index(const char* month) {
    static constexpr const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for (int i = 0; i < 12; ++i) {
        if (std::strncmp(month, months[i], 3) == 0) {
            return i;
        }
    }
    return 0;
}

bool seed_time_from_build() {
    char month_text[4] = {};
    int day = 1;
    int year = 2026;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(__DATE__, "%3s %d %d", month_text, &day, &year) != 3 ||
        std::sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return false;
    }

    tm timeinfo = {};
    timeinfo.tm_year = year - 1900;
    timeinfo.tm_mon = build_month_index(month_text);
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = second;
    std::time_t build_time = mktime(&timeinfo);
    if (build_time <= 0) {
        return false;
    }
    timeval now = {};
    now.tv_sec = build_time + static_cast<time_t>(uptime_seconds());
    settimeofday(&now, nullptr);
    tiger_log("WARN", TAG, "SNTP unavailable; system time seeded from firmware build time");
    return system_time_valid();
}

bool ensure_sntp_time() {
    if (system_time_valid()) {
        return true;
    }
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.cloudflare.com");
        esp_sntp_init();
        tiger_log("INFO", TAG, "SNTP time sync started for HTTPS webhook");
    }
    for (int i = 0; i < 20; ++i) {
        if (system_time_valid()) {
            tiger_log("INFO", TAG, "SNTP time sync complete");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    tiger_log("WARN", TAG, "SNTP time sync not ready; HTTPS webhook may fail");
    return seed_time_from_build();
}

uint64_t uptime_seconds() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
}

bool has_readable_state(const UniversalDevice& device) {
    cJSON* state = cJSON_Parse(device.state_json.c_str());
    if (!state) {
        return false;
    }
    const bool readable = cJSON_GetArraySize(state) > 0;
    cJSON_Delete(state);
    return readable;
}

int readable_device_count(const std::vector<UniversalDevice>& devices) {
    int count = 0;
    for (const auto& device : devices) {
        if (device.last_seen > 0 && has_readable_state(device)) {
            count++;
        }
    }
    return count;
}

void delayed_reboot_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

esp_err_t webhook_http_event_handler(esp_http_client_event_t* evt) {
    if (!evt->user_data) {
        return ESP_OK;
    }
    auto* trace = static_cast<WebhookHttpTrace*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        const size_t room = sizeof(trace->response) - trace->response_len - 1;
        const size_t copy_len = std::min(room, static_cast<size_t>(evt->data_len));
        if (copy_len > 0) {
            std::memcpy(trace->response + trace->response_len, evt->data, copy_len);
            trace->response_len += copy_len;
            trace->response[trace->response_len] = '\0';
        }
    } else if (evt->event_id == HTTP_EVENT_DISCONNECTED && evt->data) {
        trace->tls_capture_err = esp_tls_get_and_clear_last_error(
            static_cast<esp_tls_error_handle_t>(evt->data), &trace->mbedtls_error, &trace->tls_flags);
    }
    return ESP_OK;
}

class ScopedBleScanPause {
public:
    explicit ScopedBleScanPause(const char* reason) {
        ble_sensor_gateway().pause_auto_scan(reason);
    }
    ~ScopedBleScanPause() {
        ble_sensor_gateway().resume_auto_scan();
    }
};

cJSON* build_webhook_payload(const UniversalDevice& device) {
    auto status = device_manager().status();
    const uint64_t now_uptime = uptime_seconds();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "gateway_id", status.device_id.c_str());
    cJSON_AddStringToObject(root, "device_id", device.id.c_str());
    cJSON_AddStringToObject(root, "device", device.id.c_str());
    cJSON_AddStringToObject(root, "device_name", device.name.empty() ? device.id.c_str() : device.name.c_str());
    cJSON_AddStringToObject(root, "name", device.name.empty() ? device.id.c_str() : device.name.c_str());
    cJSON_AddStringToObject(root, "mac", device.address.c_str());
    cJSON_AddStringToObject(root, "brand", device.brand.c_str());
    cJSON_AddStringToObject(root, "model", device.model.c_str());
    cJSON_AddStringToObject(root, "protocol", device.protocol.c_str());
    cJSON_AddStringToObject(root, "location", device.location.c_str());
    cJSON_AddBoolToObject(root, "online", device.online);
    cJSON_AddNumberToObject(root, "last_seen_uptime", static_cast<double>(device.last_seen));
    cJSON_AddNumberToObject(root, "sent_at_uptime", static_cast<double>(status.uptime_seconds));
    if (system_time_valid()) {
        std::time_t now = 0;
        std::time(&now);
        cJSON_AddNumberToObject(root, "sent_at_epoch", static_cast<double>(now));
        if (device.last_seen > 0 && now_uptime >= device.last_seen) {
            cJSON_AddNumberToObject(root, "collected_at_epoch",
                                    static_cast<double>(now - static_cast<time_t>(now_uptime - device.last_seen)));
        }
    }

    cJSON* state = cJSON_Parse(device.state_json.c_str());
    if (state) {
        cJSON* temp = cJSON_GetObjectItem(state, "temperature_c");
        cJSON* probe = cJSON_GetObjectItem(state, "external_probe_temperature_c");
        cJSON* humidity = cJSON_GetObjectItem(state, "humidity_percent");
        cJSON* battery = cJSON_GetObjectItem(state, "battery_percent");
        if (cJSON_IsNumber(temp)) cJSON_AddNumberToObject(root, "temp_c", temp->valuedouble);
        if (!cJSON_IsNumber(temp) && cJSON_IsNumber(probe)) cJSON_AddNumberToObject(root, "temp_c", probe->valuedouble);
        if (cJSON_IsNumber(temp)) cJSON_AddNumberToObject(root, "temperature_c", temp->valuedouble);
        if (cJSON_IsNumber(probe)) cJSON_AddNumberToObject(root, "external_probe_temp_c", probe->valuedouble);
        if (cJSON_IsNumber(probe)) cJSON_AddNumberToObject(root, "external_probe_temperature_c", probe->valuedouble);
        if (cJSON_IsNumber(humidity)) cJSON_AddNumberToObject(root, "humid", humidity->valuedouble);
        if (cJSON_IsNumber(humidity)) cJSON_AddNumberToObject(root, "humidity_percent", humidity->valuedouble);
        if (cJSON_IsNumber(battery)) cJSON_AddNumberToObject(root, "batt", battery->valueint);
        if (cJSON_IsNumber(battery)) cJSON_AddNumberToObject(root, "battery_percent", battery->valueint);
        cJSON_Delete(state);
    }
    return root;
}

cJSON* build_heartbeat_payload(const WebhookConfig& config,
                               bool has_pending_result,
                               bool pending_ok,
                               const std::string& pending_id,
                               const std::string& pending_command,
                               const std::string& pending_message,
                               uint64_t last_webhook_send) {
    auto status = device_manager().status();
    auto wifi = wifi_manager().status();
    auto devices = device_registry().devices();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "gateway_id", status.device_id.c_str());
    cJSON_AddStringToObject(root, "device_id", status.device_id.c_str());
    cJSON_AddStringToObject(root, "firmware_version", status.firmware_version.c_str());
    cJSON_AddStringToObject(root, "build_time", status.build_time.c_str());
    cJSON_AddStringToObject(root, "lan_ip", wifi.ip_address.c_str());
    cJSON_AddStringToObject(root, "wifi_ssid", wifi.ssid.c_str());
    cJSON_AddNumberToObject(root, "wifi_rssi", wifi.rssi);
    cJSON_AddNumberToObject(root, "uptime_seconds", static_cast<double>(status.uptime_seconds));
    cJSON_AddNumberToObject(root, "free_heap", status.free_heap);
    cJSON_AddNumberToObject(root, "minimum_free_heap", status.minimum_free_heap);
    cJSON_AddNumberToObject(root, "sensor_count", static_cast<double>(devices.size()));
    cJSON_AddNumberToObject(root, "readable_sensor_count", readable_device_count(devices));
    cJSON_AddBoolToObject(root, "webhook_enabled", config.enabled);
    cJSON_AddNumberToObject(root, "webhook_last_send_seconds", static_cast<double>(last_webhook_send));
    cJSON_AddStringToObject(root, "status", "online");

    if (system_time_valid()) {
        std::time_t now = 0;
        std::time(&now);
        cJSON_AddNumberToObject(root, "sent_at_epoch", static_cast<double>(now));
    }

    // Command results are acknowledged through the next heartbeat so the cloud
    // can move a claimed command to completed/failed without opening an inbound
    // connection to the ESP32 inside the store network.
    cJSON* results = cJSON_AddArrayToObject(root, "command_results");
    if (has_pending_result && !pending_id.empty()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", pending_id.c_str());
        cJSON_AddStringToObject(item, "command", pending_command.c_str());
        cJSON_AddBoolToObject(item, "ok", pending_ok);
        cJSON_AddStringToObject(item, "message", pending_message.c_str());
        cJSON_AddNumberToObject(item, "completed_at_uptime", static_cast<double>(status.uptime_seconds));
        cJSON_AddItemToArray(results, item);
    }
    return root;
}

} // namespace

esp_err_t CloudClient::init() {
    if (initialized_) {
        return ESP_OK;
    }
    initialized_ = true;
    if (!task_started_) {
        xTaskCreate(task_entry, "cloud_client", 8192, this, 4, nullptr);
        task_started_ = true;
    }
    tiger_log("INFO", TAG, "Cloud client initialized with HTTP webhook support");
    return ESP_OK;
}

esp_err_t CloudClient::loop_once() {
    WebhookConfig cfg = nvs_store().get_webhook_config();
    if (!cfg.enabled || cfg.url.empty() || !wifi_manager().status().connected) {
        return ESP_OK;
    }
    const uint64_t now = uptime_seconds();
    if (now - last_heartbeat_send_ >= HEARTBEAT_INTERVAL_SECONDS) {
        last_heartbeat_send_ = now;
        send_heartbeat(cfg);
    }
    if (webhook_send_in_progress_) {
        return ESP_OK;
    }
    if (now - last_webhook_send_ < static_cast<uint64_t>(cfg.interval_seconds)) {
        return ESP_OK;
    }
    last_webhook_send_ = now;
    return queue_webhook_send();
}

WebhookConfig CloudClient::config() const {
    return nvs_store().get_webhook_config();
}

esp_err_t CloudClient::save_webhook_config(const WebhookConfig& config) {
    return nvs_store().save_webhook_config(config);
}

esp_err_t CloudClient::send_webhook_now(WebhookSendResult* result) {
    if (webhook_send_in_progress_) {
        if (result) result->err = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    WebhookConfig cfg = nvs_store().get_webhook_config();
    if (!cfg.enabled || cfg.url.empty()) {
        if (result) result->err = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager().status().connected) {
        if (result) result->err = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    last_webhook_send_ = uptime_seconds();
    webhook_send_in_progress_ = true;
    WebhookSendResult send_result = send_device_webhook(cfg);
    webhook_send_in_progress_ = false;
    if (result) {
        *result = send_result;
    }
    return send_result.err;
}

esp_err_t CloudClient::queue_webhook_send() {
    if (webhook_send_in_progress_) {
        return ESP_ERR_INVALID_STATE;
    }
    WebhookConfig cfg = nvs_store().get_webhook_config();
    if (!cfg.enabled || cfg.url.empty() || !wifi_manager().status().connected) {
        return ESP_ERR_INVALID_STATE;
    }
    webhook_send_in_progress_ = true;
    BaseType_t ok = xTaskCreate(webhook_send_task_entry, "webhook_send", 8192, this, 4, nullptr);
    if (ok != pdPASS) {
        webhook_send_in_progress_ = false;
        return ESP_ERR_NO_MEM;
    }
    tiger_log("INFO", TAG, "Webhook send queued");
    return ESP_OK;
}

void CloudClient::task_entry(void* arg) {
    static_cast<CloudClient*>(arg)->task_loop();
}

void CloudClient::webhook_send_task_entry(void* arg) {
    CloudClient* self = static_cast<CloudClient*>(arg);
    self->last_webhook_send_ = uptime_seconds();
    WebhookConfig cfg = nvs_store().get_webhook_config();
    WebhookSendResult result = self->send_device_webhook(cfg);
    char message[128];
    std::snprintf(message, sizeof(message), "Webhook background send complete err=%s sent=%d skipped=%d failed=%d",
                  esp_err_to_name(result.err), result.sent_count, result.skipped_count, result.failed_count);
    tiger_log(result.err == ESP_OK ? "INFO" : "WARN", TAG, message);
    self->webhook_send_in_progress_ = false;
    vTaskDelete(nullptr);
}

void CloudClient::task_loop() {
    while (true) {
        loop_once();
        vTaskDelay(pdMS_TO_TICKS(CLOUD_TASK_DELAY_MS));
    }
}

esp_err_t CloudClient::send_heartbeat(const WebhookConfig& config) {
    const std::string url = heartbeat_url_from_webhook_url(config.url);
    if (url.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (starts_with(url, "https://") && !ensure_sntp_time()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON* root = build_heartbeat_payload(config,
                                          has_pending_command_result_,
                                          pending_command_ok_,
                                          pending_command_id_,
                                          pending_command_name_,
                                          pending_command_message_,
                                          last_webhook_send_);
    char* text = cJSON_PrintUnformatted(root);
    if (!text) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    WebhookHttpTrace trace = {};
    esp_http_client_config_t http_config = {};
    http_config.url = url.c_str();
    http_config.method = HTTP_METHOD_POST;
    http_config.timeout_ms = HTTP_TIMEOUT_MS;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.user_agent = "TigerOS/1.0 ESP32-S3";
    http_config.buffer_size = 768;
    http_config.buffer_size_tx = 768;
    http_config.event_handler = webhook_http_event_handler;
    http_config.user_data = &trace;

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        cJSON_free(text);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (!config.secret_header.empty() && !config.secret_value.empty()) {
        esp_http_client_set_header(client, config.secret_header.c_str(), config.secret_value.c_str());
    }
    esp_http_client_set_post_field(client, text, std::strlen(text));
    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    const int transport_errno = esp_http_client_get_errno(client);
    esp_http_client_cleanup(client);
    cJSON_free(text);
    cJSON_Delete(root);

    if (err != ESP_OK || status < 200 || status >= 300) {
        char message[384];
        std::snprintf(message, sizeof(message),
                      "Cloud heartbeat failed status=%d err=%s errno=%d tls=0x%x flags=0x%x response=%.220s",
                      status, esp_err_to_name(err == ESP_OK ? ESP_FAIL : err), transport_errno,
                      trace.mbedtls_error, trace.tls_flags, trace.response);
        tiger_log("WARN", TAG, message);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    if (has_pending_command_result_) {
        has_pending_command_result_ = false;
        pending_command_id_.clear();
        pending_command_name_.clear();
        pending_command_message_.clear();
    }

    cJSON* response = cJSON_Parse(trace.response);
    if (!response) {
        tiger_log("WARN", TAG, "Cloud heartbeat response was not valid JSON");
        return ESP_OK;
    }
    cJSON* commands = cJSON_GetObjectItem(response, "commands");
    if (cJSON_IsArray(commands)) {
        handle_cloud_commands(commands);
    }
    cJSON_Delete(response);
    tiger_log("INFO", TAG, "Cloud heartbeat sent");
    return ESP_OK;
}

void CloudClient::record_command_result(const char* id, const char* command, bool ok, const char* message) {
    pending_command_id_ = id ? id : "";
    pending_command_name_ = command ? command : "";
    pending_command_ok_ = ok;
    pending_command_message_ = message ? message : "";
    has_pending_command_result_ = !pending_command_id_.empty();
}

void CloudClient::handle_cloud_commands(cJSON* commands) {
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, commands) {
        cJSON* id = cJSON_GetObjectItem(item, "id");
        cJSON* command = cJSON_GetObjectItem(item, "command");
        if (!cJSON_IsString(id) || !cJSON_IsString(command)) {
            continue;
        }

        char log_message[160];
        std::snprintf(log_message, sizeof(log_message), "Cloud command received: %s", command->valuestring);
        tiger_log("INFO", TAG, log_message);

        if (std::strcmp(command->valuestring, "sync_webhook") == 0) {
            esp_err_t err = queue_webhook_send();
            record_command_result(id->valuestring, command->valuestring, err == ESP_OK,
                                  err == ESP_OK ? "Webhook sync queued" : esp_err_to_name(err));
        } else if (std::strcmp(command->valuestring, "reboot") == 0) {
            record_command_result(id->valuestring, command->valuestring, true, "Reboot scheduled");
            xTaskCreate(delayed_reboot_task, "cloud_reboot", 2048, nullptr, 5, nullptr);
        } else if (std::strcmp(command->valuestring, "ota_check") == 0) {
            record_command_result(id->valuestring, command->valuestring, false,
                                  "Remote OTA check command is not enabled in this build");
        } else if (std::strcmp(command->valuestring, "upload_logs") == 0) {
            record_command_result(id->valuestring, command->valuestring, false,
                                  "Log upload command is not enabled in this build");
        } else {
            record_command_result(id->valuestring, command->valuestring, false, "Unknown command");
        }
        return;
    }
}

WebhookSendResult CloudClient::send_device_webhook(const WebhookConfig& config) {
    WebhookSendResult result;
    if (starts_with(config.url, "https://") && !ensure_sntp_time()) {
        result.err = ESP_ERR_INVALID_STATE;
        return result;
    }
    ScopedBleScanPause pause("webhook HTTPS upload");
    for (const auto& device : device_registry().devices()) {
        // Webhook is the bridge into external systems, so watched devices with
        // cached readable BLE data should still report their last known state.
        // The payload keeps the online flag, allowing the receiver to distinguish
        // fresh live readings from cached/offline readings.
        if (device.last_seen == 0 || !has_readable_state(device)) {
            result.skipped_count++;
            continue;
        }

        cJSON* root = build_webhook_payload(device);
        char* text = cJSON_PrintUnformatted(root);
        if (!text) {
            cJSON_Delete(root);
            result.err = ESP_ERR_NO_MEM;
            result.failed_count++;
            continue;
        }

        WebhookHttpTrace trace = {};
        esp_http_client_config_t http_config = {};
        http_config.url = config.url.c_str();
        http_config.method = HTTP_METHOD_POST;
        http_config.timeout_ms = HTTP_TIMEOUT_MS;
        http_config.crt_bundle_attach = esp_crt_bundle_attach;
        http_config.user_agent = "TigerOS/1.0 ESP32-S3";
        http_config.buffer_size = 512;
        http_config.buffer_size_tx = 512;
        http_config.event_handler = webhook_http_event_handler;
        http_config.user_data = &trace;
        esp_http_client_handle_t client = esp_http_client_init(&http_config);
        if (!client) {
            cJSON_free(text);
            cJSON_Delete(root);
            result.err = ESP_FAIL;
            result.failed_count++;
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        if (!config.secret_header.empty() && !config.secret_value.empty()) {
            esp_http_client_set_header(client, config.secret_header.c_str(), config.secret_value.c_str());
        }
        esp_http_client_set_post_field(client, text, std::strlen(text));
        char start_message[180];
        std::snprintf(start_message, sizeof(start_message), "Webhook POST start device=%s bytes=%u heap=%lu",
                      device.id.c_str(), static_cast<unsigned>(std::strlen(text)),
                      static_cast<unsigned long>(esp_get_free_heap_size()));
        tiger_log("INFO", TAG, start_message);
        esp_err_t err = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        const int transport_errno = esp_http_client_get_errno(client);
        esp_http_client_cleanup(client);
        cJSON_free(text);
        cJSON_Delete(root);

        if (err == ESP_OK && status >= 200 && status < 300) {
            result.sent_count++;
            continue;
        }

        result.err = err == ESP_OK ? ESP_FAIL : err;
        result.failed_count++;
        char message[384];
        std::snprintf(message, sizeof(message),
                      "Webhook POST failed device=%s status=%d err=%s errno=%d tls=0x%x flags=0x%x response=%.180s heap=%lu",
                      device.id.c_str(), status, esp_err_to_name(result.err), transport_errno,
                      trace.mbedtls_error, trace.tls_flags, trace.response,
                      static_cast<unsigned long>(esp_get_free_heap_size()));
        tiger_log("WARN", TAG, message);
    }

    if (result.sent_count > 0) {
        char message[128];
        std::snprintf(message, sizeof(message), "Webhook posted device_count=%d", result.sent_count);
        tiger_log("INFO", TAG, message);
    } else if (result.failed_count == 0) {
        result.err = ESP_ERR_NOT_FOUND;
        tiger_log("WARN", TAG, "Webhook skipped: no readable devices");
    }
    return result;
}

CloudClient& cloud_client() {
    static CloudClient client;
    return client;
}

} // namespace tigeros
