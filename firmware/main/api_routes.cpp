#include "api_routes.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "auth_manager.h"
#include "board_support.h"
#include "ble_manager.h"
#include "ble_sensor_gateway.h"
#include "cJSON.h"
#include "cloud_client.h"
#include "device_manager.h"
#include "device_registry.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_manager.h"
#include "nvs_store.h"
#include "nvs.h"
#include "ota_manager.h"
#include "tiger_log.h"
#include "tft_display.h"
#include "wifi_manager.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "api_routes";
constexpr size_t MAX_JSON_BODY = 4096;

const char* status_text(int status) {
    switch (status) {
        case 200:
            return "200 OK";
        case 400:
            return "400 Bad Request";
        case 401:
            return "401 Unauthorized";
        case 403:
            return "403 Forbidden";
        case 404:
            return "404 Not Found";
        case 409:
            return "409 Conflict";
        case 500:
            return "500 Internal Server Error";
        default:
            return "500 Internal Server Error";
    }
}

esp_err_t send_json(httpd_req_t* req, const char* json, int status = 200) {
    httpd_resp_set_status(req, status_text(status));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}

esp_err_t send_error(httpd_req_t* req, int status, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text, status);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

bool authorized(httpd_req_t* req) {
    return auth_manager().is_authorized(req);
}

esp_err_t require_auth(httpd_req_t* req) {
    return send_error(req, 401, "Authorization bearer token is required");
}

std::string read_body(httpd_req_t* req, size_t max_size = MAX_JSON_BODY) {
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > max_size) {
        return {};
    }

    std::string body;
    body.resize(req->content_len);
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body.data() + received, req->content_len - received);
        if (ret <= 0) {
            return {};
        }
        received += ret;
    }
    return body;
}

void delayed_restart_task(void* arg) {
    auto delay_ms = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

void restart_after_response(uint32_t delay_ms) {
    xTaskCreate(
        delayed_restart_task,
        "delayed_restart",
        2048,
        reinterpret_cast<void*>(static_cast<uintptr_t>(delay_ms)),
        5,
        nullptr
    );
}

void add_ble_sensor_json(cJSON* item, const BleSensorReading& reading, bool include_raw) {
    cJSON_AddStringToObject(item, "mac", reading.mac_address.c_str());
    cJSON_AddStringToObject(item, "sensor_mac", reading.mac_address.c_str());
    cJSON_AddStringToObject(item, "name", reading.name.c_str());
    cJSON_AddStringToObject(item, "sensor_name", reading.display_name.empty() ? reading.name.c_str() : reading.display_name.c_str());
    cJSON_AddStringToObject(item, "display_name", reading.display_name.c_str());
    cJSON_AddStringToObject(item, "brand", reading.brand.c_str());
    cJSON_AddStringToObject(item, "model", reading.model.c_str());
    cJSON_AddStringToObject(item, "protocol", reading.protocol.c_str());
    cJSON_AddStringToObject(item, "sensor_type", reading.protocol.c_str());
    cJSON_AddStringToObject(item, "location", reading.location.c_str());
    cJSON_AddStringToObject(item, "parse_status", reading.parse_status.c_str());
    cJSON_AddBoolToObject(item, "paired", reading.paired);
    cJSON_AddNumberToObject(item, "rssi", reading.rssi);
    cJSON_AddNumberToObject(item, "last_seen", static_cast<double>(reading.last_seen));
    if (reading.has_temperature) cJSON_AddNumberToObject(item, "temperature_c", reading.temperature_c);
    if (reading.has_humidity) cJSON_AddNumberToObject(item, "humidity_percent", reading.humidity_percent);
    if (reading.has_battery) cJSON_AddNumberToObject(item, "battery_percent", reading.battery_percent);
    if (reading.has_external_probe_temperature) {
        cJSON_AddNumberToObject(item, "external_probe_temperature_c", reading.external_probe_temperature_c);
    }
    if (include_raw) {
        cJSON_AddStringToObject(item, "raw_advertisement", reading.raw_advertisement_hex.c_str());
        cJSON_AddStringToObject(item, "raw_adv_hex", reading.raw_advertisement_hex.c_str());
        cJSON_AddStringToObject(item, "debug", reading.debug.c_str());
    }
}

void add_ble_raw_packet_json(cJSON* item, const BleRawPacket& packet) {
    cJSON_AddStringToObject(item, "mac", packet.mac_address.c_str());
    cJSON_AddStringToObject(item, "sensor_mac", packet.mac_address.c_str());
    cJSON_AddStringToObject(item, "address_type", packet.address_type.c_str());
    cJSON_AddStringToObject(item, "name", packet.name.c_str());
    cJSON_AddStringToObject(item, "sensor_name", packet.name.c_str());
    cJSON_AddStringToObject(item, "brand", packet.brand.c_str());
    cJSON_AddStringToObject(item, "model", packet.model.c_str());
    cJSON_AddStringToObject(item, "protocol", packet.protocol.c_str());
    cJSON_AddStringToObject(item, "sensor_type", packet.protocol.c_str());
    cJSON_AddStringToObject(item, "parse_status", packet.parse_status.c_str());
    cJSON_AddNumberToObject(item, "rssi", packet.rssi);
    cJSON_AddNumberToObject(item, "last_seen", static_cast<double>(packet.last_seen));
    if (packet.has_temperature) cJSON_AddNumberToObject(item, "temperature_c", packet.temperature_c);
    if (packet.has_humidity) cJSON_AddNumberToObject(item, "humidity_percent", packet.humidity_percent);
    if (packet.has_battery) cJSON_AddNumberToObject(item, "battery_percent", packet.battery_percent);
    if (packet.has_external_probe_temperature) {
        cJSON_AddNumberToObject(item, "external_probe_temperature_c", packet.external_probe_temperature_c);
    }
    cJSON_AddStringToObject(item, "raw_advertisement", packet.raw_advertisement_hex.c_str());
    cJSON_AddStringToObject(item, "raw_adv_hex", packet.raw_advertisement_hex.c_str());
    cJSON_AddStringToObject(item, "manufacturer_data_hex", packet.manufacturer_data_hex.c_str());
    cJSON_AddStringToObject(item, "service_data_hex", packet.service_data_hex.c_str());
    cJSON* uuids = cJSON_AddArrayToObject(item, "service_uuids");
    for (const auto& uuid : packet.service_uuids) {
        cJSON_AddItemToArray(uuids, cJSON_CreateString(uuid.c_str()));
    }
    cJSON_AddStringToObject(item, "debug", packet.debug.c_str());
}

bool ble_raw_packet_has_values(const BleRawPacket& packet) {
    return packet.has_temperature || packet.has_humidity || packet.has_battery || packet.has_external_probe_temperature;
}

std::string gatt_properties_text(uint8_t properties) {
    std::string text;
    auto add = [&](uint8_t bit, const char* name) {
        if ((properties & bit) == 0) return;
        if (!text.empty()) text += ",";
        text += name;
    };
    add(0x02, "read");
    add(0x04, "write_no_rsp");
    add(0x08, "write");
    add(0x10, "notify");
    add(0x20, "indicate");
    return text.empty() ? "-" : text;
}

void add_ble_gatt_inspection_json(cJSON* root, const BleGattInspection& inspection) {
    // The HTTP request can succeed while the BLE inspection is still running
    // or has failed. Keep API transport success separate from GATT result so
    // the Web Console can poll without showing a generic request failure.
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "inspection_ok", inspection.ok);
    cJSON_AddBoolToObject(root, "running", inspection.running);
    cJSON_AddBoolToObject(root, "connected", inspection.connected);
    cJSON_AddNumberToObject(root, "error_code", inspection.error_code);
    cJSON_AddStringToObject(root, "error", inspection.error.c_str());
    cJSON_AddStringToObject(root, "mac", inspection.mac_address.c_str());
    cJSON_AddStringToObject(root, "address_type", inspection.address_type.c_str());
    cJSON_AddNumberToObject(root, "started_at", static_cast<double>(inspection.started_at));
    cJSON_AddNumberToObject(root, "completed_at", static_cast<double>(inspection.completed_at));
    cJSON* services = cJSON_AddArrayToObject(root, "services");
    for (const auto& service : inspection.services) {
        cJSON* svc = cJSON_CreateObject();
        cJSON_AddStringToObject(svc, "uuid", service.uuid.c_str());
        cJSON_AddNumberToObject(svc, "start_handle", service.start_handle);
        cJSON_AddNumberToObject(svc, "end_handle", service.end_handle);
        cJSON* characteristics = cJSON_AddArrayToObject(svc, "characteristics");
        for (const auto& chr : service.characteristics) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "uuid", chr.uuid.c_str());
            cJSON_AddNumberToObject(item, "definition_handle", chr.definition_handle);
            cJSON_AddNumberToObject(item, "value_handle", chr.value_handle);
            cJSON_AddNumberToObject(item, "properties", chr.properties);
            cJSON_AddStringToObject(item, "properties_text", gatt_properties_text(chr.properties).c_str());
            cJSON_AddBoolToObject(item, "read_attempted", chr.read_attempted);
            cJSON_AddBoolToObject(item, "read_ok", chr.read_ok);
            cJSON_AddNumberToObject(item, "read_error", chr.read_error);
            cJSON_AddStringToObject(item, "value_hex", chr.value_hex.c_str());
            cJSON_AddItemToArray(characteristics, item);
        }
        cJSON_AddItemToArray(services, svc);
    }
}

void remote_ota_update_task(void* arg) {
    std::unique_ptr<RemoteOtaCheckResult> update(static_cast<RemoteOtaCheckResult*>(arg));
    esp_err_t err = ota_manager().install_remote_update(*update);
    if (err == ESP_OK) {
        tiger_log("INFO", TAG, "Remote OTA background install complete; rebooting");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    tiger_log("ERROR", TAG, "Remote OTA background install failed");
    vTaskDelete(nullptr);
}

esp_err_t status_handler(httpd_req_t* req) {
    auto status = device_manager().status();
    const bool is_authorized = authorized(req);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "firmware_version", status.firmware_version.c_str());
    cJSON_AddStringToObject(root, "build_time", status.build_time.c_str());
    cJSON_AddStringToObject(root, "wifi_mode", status.wifi_mode.c_str());
    cJSON_AddStringToObject(root, "ip_address", status.ip_address.c_str());
    cJSON_AddBoolToObject(root, "wifi_connected", status.wifi_connected);
    cJSON_AddNumberToObject(root, "uptime_seconds", static_cast<double>(status.uptime_seconds));
    cJSON_AddNumberToObject(root, "free_heap", status.free_heap);
    cJSON_AddStringToObject(root, "hardware_model", status.hardware_model.c_str());
    cJSON_AddNumberToObject(root, "flash_size", status.flash_size);
    cJSON_AddStringToObject(root, "chip_model", status.chip_model.c_str());
    if (!is_authorized && !status.wifi_connected && !status.ap_ssid.empty()) {
        cJSON_AddStringToObject(root, "ap_ssid", status.ap_ssid.c_str());
    }
    if (is_authorized) {
        cJSON_AddStringToObject(root, "device_id", status.device_id.c_str());
        cJSON_AddStringToObject(root, "wifi_ssid", status.wifi_ssid.c_str());
        cJSON_AddStringToObject(root, "ap_ssid", status.ap_ssid.c_str());
        cJSON_AddNumberToObject(root, "minimum_free_heap", status.minimum_free_heap);
        cJSON_AddStringToObject(root, "ota_channel", nvs_store().get_ota_channel().c_str());
        cJSON_AddItemToObject(root, "board", board_support().to_json());
    }

    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t login_handler(httpd_req_t* req) {
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }

    cJSON* username = cJSON_GetObjectItem(root, "username");
    cJSON* password = cJSON_GetObjectItem(root, "password");
    const bool ok = cJSON_IsString(username) && cJSON_IsString(password) &&
                    auth_manager().verify_login(username->valuestring, password->valuestring);
    cJSON_Delete(root);
    if (!ok) {
        return send_error(req, 401, "Invalid username or password");
    }

    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddStringToObject(response, "token", auth_manager().api_token().c_str());
    cJSON_AddStringToObject(response, "message", "Login successful");
    char* text = cJSON_PrintUnformatted(response);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(response);
    return err;
}

esp_err_t wifi_handler(httpd_req_t* req) {
    // First-time onboarding must work before the user has a session token.
    // Once station WiFi is connected, changing credentials requires auth.
    const bool setup_mode = !wifi_manager().status().connected;
    if (!setup_mode && !authorized(req)) {
        return require_auth(req);
    }

    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }

    cJSON* ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON* password = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        cJSON_Delete(root);
        return send_error(req, 400, "SSID is required");
    }

    esp_err_t err = wifi_manager().save_and_connect(ssid->valuestring, cJSON_IsString(password) ? password->valuestring : "");
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_and_connect failed: %s", esp_err_to_name(err));
        return send_error(req, 500, "Failed to save WiFi credentials");
    }
    tiger_log("INFO", TAG, "WiFi credentials updated");
    return send_json(req, "{\"ok\":true,\"message\":\"WiFi credentials saved\"}");
}

esp_err_t wifi_scan_handler(httpd_req_t* req) {
    auto networks = wifi_manager().scan_networks();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* items = cJSON_AddArrayToObject(root, "networks");

    for (const auto& network : networks) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", network.ssid.c_str());
        cJSON_AddNumberToObject(item, "rssi", network.rssi);
        cJSON_AddNumberToObject(item, "channel", network.channel);
        cJSON_AddStringToObject(item, "auth_mode", network.auth_mode.c_str());
        cJSON_AddItemToArray(items, item);
    }

    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t wifi_forget_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    esp_err_t clear_err = nvs_store().clear_wifi_credentials();
    if (clear_err != ESP_OK) {
        ESP_LOGE(TAG, "clear_wifi_credentials failed: %s", esp_err_to_name(clear_err));
        return send_error(req, 500, "Failed to forget WiFi credentials");
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    esp_err_t restore_err = esp_wifi_restore();
    if (restore_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_restore failed: %s", esp_err_to_name(restore_err));
        return send_error(req, 500, "Failed to clear WiFi driver credentials");
    }
    tiger_log("WARN", TAG, "WiFi credentials forgotten from Web Console");
    send_json(req, "{\"ok\":true,\"message\":\"WiFi credentials forgotten; rebooting into setup AP\",\"reboot_ms\":1000}");
    restart_after_response(1000);
    return ESP_OK;
}

esp_err_t reboot_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    tiger_log("WARN", TAG, "Reboot requested from Web Console");
    send_json(req, "{\"ok\":true,\"message\":\"Rebooting\",\"reboot_ms\":1000}");
    restart_after_response(1000);
    return ESP_OK;
}

esp_err_t factory_reset_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    esp_err_t reset_err = nvs_store().factory_reset_config();
    if (reset_err != ESP_OK) {
        return send_error(req, 500, "Factory reset failed");
    }
    tiger_log("WARN", TAG, "Factory reset requested from Web Console");
    send_json(req, "{\"ok\":true,\"message\":\"Factory reset complete; rebooting\",\"reboot_ms\":1000}");
    restart_after_response(1000);
    return ESP_OK;
}

esp_err_t ota_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    esp_err_t err = ota_manager().handle_upload(req);
    if (err != ESP_OK) {
        return send_error(req, 500, "OTA update failed");
    }
    send_json(req, "{\"ok\":true,\"message\":\"OTA update installed; rebooting\",\"reboot_ms\":1500}");
    restart_after_response(1500);
    return ESP_OK;
}

esp_err_t logs_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* items = cJSON_AddArrayToObject(root, "logs");
    for (const auto& log : tiger_logs()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "uptime_seconds", static_cast<double>(log.uptime_seconds));
        cJSON_AddStringToObject(item, "level", log.level.c_str());
        cJSON_AddStringToObject(item, "tag", log.tag.c_str());
        cJSON_AddStringToObject(item, "message", log.message.c_str());
        cJSON_AddItemToArray(items, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t logs_clear_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    tiger_logs_clear();
    tiger_log("INFO", TAG, "Log ring buffer cleared from Web Console");
    return send_json(req, "{\"ok\":true,\"message\":\"Logs cleared\"}");
}

esp_err_t control_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    std::string body = read_body(req);
    ESP_LOGI(TAG, "Control request: %s", body.c_str());
    tiger_log("INFO", TAG, "Control command accepted");
    return send_json(req, "{\"ok\":true,\"message\":\"Control command accepted\",\"placeholder\":true}");
}

esp_err_t ota_check_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    auto status = device_manager().status();
    auto ota_config = nvs_store().get_ota_config();
    RemoteOtaConfig config;
    config.enabled = ota_config.enabled;
    config.auto_update = ota_config.auto_update;
    config.ota_check_url = ota_config.check_url;
    config.current_version = status.firmware_version;
    config.device_id = status.device_id;
    config.channel = ota_config.channel;
    config.hardware_model = status.hardware_model;
    config.flash_size = status.flash_size;
    config.chip_model = status.chip_model;
    auto result = ota_manager().check_remote_update(config);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", result.ok);
    cJSON_AddBoolToObject(root, "update_available", result.update_available);
    cJSON_AddStringToObject(root, "version", result.version.c_str());
    cJSON_AddStringToObject(root, "firmware_url", result.firmware_url.c_str());
    cJSON_AddStringToObject(root, "sha256", result.sha256.c_str());
    cJSON_AddStringToObject(root, "release_notes", result.release_notes.c_str());
    cJSON_AddBoolToObject(root, "force", result.force);
    if (!result.error.empty()) cJSON_AddStringToObject(root, "error", result.error.c_str());
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text, result.ok ? 200 : 500);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t ota_config_get_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    auto config = nvs_store().get_ota_config();
    auto status = device_manager().status();
    auto last = ota_manager().last_remote_check();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "ota_enabled", config.enabled);
    cJSON_AddBoolToObject(root, "auto_update", config.auto_update);
    cJSON_AddStringToObject(root, "ota_check_url", config.check_url.c_str());
    cJSON_AddStringToObject(root, "ota_channel", config.channel.c_str());
    cJSON_AddStringToObject(root, "current_version", status.firmware_version.c_str());
    cJSON_AddStringToObject(root, "device_id", status.device_id.c_str());
    cJSON_AddStringToObject(root, "hardware_model", status.hardware_model.c_str());
    cJSON_AddNumberToObject(root, "flash_size", status.flash_size);
    cJSON_AddStringToObject(root, "chip_model", status.chip_model.c_str());
    cJSON_AddBoolToObject(root, "device_token_set", !nvs_store().get_cloud_token().empty());
    cJSON_AddNumberToObject(root, "progress", ota_manager().remote_progress());
    cJSON_AddBoolToObject(root, "update_available", last.update_available);
    cJSON_AddStringToObject(root, "latest_version", last.version.c_str());
    cJSON_AddStringToObject(root, "firmware_url", last.firmware_url.c_str());
    cJSON_AddStringToObject(root, "sha256", last.sha256.c_str());
    cJSON_AddStringToObject(root, "release_notes", last.release_notes.c_str());
    cJSON_AddBoolToObject(root, "force", last.force);
    if (!last.error.empty()) cJSON_AddStringToObject(root, "last_error", last.error.c_str());
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t ota_config_post_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }

    OtaConfig next = nvs_store().get_ota_config();
    cJSON* enabled = cJSON_GetObjectItem(root, "ota_enabled");
    cJSON* auto_update = cJSON_GetObjectItem(root, "auto_update");
    cJSON* url = cJSON_GetObjectItem(root, "ota_check_url");
    cJSON* channel = cJSON_GetObjectItem(root, "ota_channel");
    cJSON* device_token = cJSON_GetObjectItem(root, "device_token");
    if (cJSON_IsBool(enabled)) next.enabled = cJSON_IsTrue(enabled);
    if (cJSON_IsBool(auto_update)) next.auto_update = cJSON_IsTrue(auto_update);
    if (cJSON_IsString(url)) next.check_url = url->valuestring;
    if (cJSON_IsString(channel)) next.channel = channel->valuestring;
    std::string token;
    if (cJSON_IsString(device_token) && device_token->valuestring[0] != '\0') {
        token = device_token->valuestring;
    }
    cJSON_Delete(root);

    esp_err_t err = nvs_store().save_ota_config(next);
    if (err == ESP_OK && !token.empty()) {
        err = nvs_store().save_cloud_token(token);
    }
    if (err != ESP_OK) {
        return send_error(req, 400, "Invalid OTA configuration");
    }
    tiger_log("INFO", TAG, "Cloud OTA configuration saved");
    return send_json(req, "{\"ok\":true,\"message\":\"OTA configuration saved\"}");
}

esp_err_t ota_update_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    RemoteOtaCheckResult update = ota_manager().last_remote_check();
    if (req->content_len > 0) {
        std::string body = read_body(req);
        cJSON* root = cJSON_Parse(body.c_str());
        if (root) {
            cJSON* firmware_url = cJSON_GetObjectItem(root, "firmware_url");
            cJSON* sha256 = cJSON_GetObjectItem(root, "sha256");
            cJSON* version = cJSON_GetObjectItem(root, "version");
            if (cJSON_IsString(firmware_url)) update.firmware_url = firmware_url->valuestring;
            if (cJSON_IsString(sha256)) update.sha256 = sha256->valuestring;
            if (cJSON_IsString(version)) update.version = version->valuestring;
            cJSON_Delete(root);
        }
    }
    update.update_available = true;

    auto* task_update = new RemoteOtaCheckResult(update);
    if (xTaskCreate(remote_ota_update_task, "remote_ota", 8192, task_update, 5, nullptr) != pdPASS) {
        delete task_update;
        return send_error(req, 500, "Failed to start remote OTA task");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"Remote OTA install started\"}");
}

esp_err_t mqtt_get_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    MqttConfig config = mqtt_manager().config();
    MqttStatus status = mqtt_manager().status();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "enabled", config.enabled);
    cJSON_AddStringToObject(root, "host", config.host.c_str());
    cJSON_AddNumberToObject(root, "port", config.port);
    cJSON_AddStringToObject(root, "username", config.username.c_str());
    cJSON_AddStringToObject(root, "client_id", config.client_id.c_str());
    cJSON_AddBoolToObject(root, "use_tls", config.use_tls);
    cJSON_AddBoolToObject(root, "ha_discovery_enabled", config.ha_discovery_enabled);
    cJSON_AddStringToObject(root, "ha_discovery_prefix", config.ha_discovery_prefix.c_str());
    cJSON_AddStringToObject(root, "state", status.state_text.c_str());
    cJSON_AddBoolToObject(root, "password_set", !config.password.empty());
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t mqtt_post_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }

    MqttConfig current = mqtt_manager().config();
    MqttConfig next = current;
    cJSON* enabled = cJSON_GetObjectItem(root, "enabled");
    cJSON* host = cJSON_GetObjectItem(root, "host");
    cJSON* port = cJSON_GetObjectItem(root, "port");
    cJSON* username = cJSON_GetObjectItem(root, "username");
    cJSON* password = cJSON_GetObjectItem(root, "password");
    cJSON* client_id = cJSON_GetObjectItem(root, "client_id");
    cJSON* use_tls = cJSON_GetObjectItem(root, "use_tls");
    cJSON* ha_discovery_enabled = cJSON_GetObjectItem(root, "ha_discovery_enabled");
    cJSON* ha_discovery_prefix = cJSON_GetObjectItem(root, "ha_discovery_prefix");

    if (cJSON_IsBool(enabled)) next.enabled = cJSON_IsTrue(enabled);
    if (cJSON_IsString(host)) next.host = host->valuestring;
    if (cJSON_IsNumber(port)) next.port = port->valueint;
    if (cJSON_IsString(username)) next.username = username->valuestring;
    if (cJSON_IsString(password) && password->valuestring[0] != '\0') next.password = password->valuestring;
    if (cJSON_IsString(client_id)) next.client_id = client_id->valuestring;
    if (cJSON_IsBool(use_tls)) next.use_tls = cJSON_IsTrue(use_tls);
    if (cJSON_IsBool(ha_discovery_enabled)) next.ha_discovery_enabled = cJSON_IsTrue(ha_discovery_enabled);
    if (cJSON_IsString(ha_discovery_prefix)) next.ha_discovery_prefix = ha_discovery_prefix->valuestring;
    if (next.client_id.empty()) next.client_id = nvs_store().get_or_create_device_id();
    if (next.ha_discovery_prefix.empty()) next.ha_discovery_prefix = "homeassistant";
    if (next.port <= 0) next.port = next.use_tls ? 8883 : 1883;

    cJSON_Delete(root);

    esp_err_t err = mqtt_manager().apply_config(next);
    if (err != ESP_OK) {
        return send_error(req, 400, "Invalid MQTT configuration");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"MQTT settings saved\"}");
}

esp_err_t webhook_get_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    WebhookConfig config = cloud_client().config();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "enabled", config.enabled);
    cJSON_AddStringToObject(root, "url", config.url.c_str());
    cJSON_AddStringToObject(root, "secret_header", config.secret_header.c_str());
    cJSON_AddBoolToObject(root, "secret_set", !config.secret_value.empty());
    cJSON_AddNumberToObject(root, "interval_seconds", config.interval_seconds);
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t webhook_post_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }

    WebhookConfig next = cloud_client().config();
    cJSON* enabled = cJSON_GetObjectItem(root, "enabled");
    cJSON* url = cJSON_GetObjectItem(root, "url");
    cJSON* secret_header = cJSON_GetObjectItem(root, "secret_header");
    cJSON* secret_value = cJSON_GetObjectItem(root, "secret_value");
    cJSON* interval_seconds = cJSON_GetObjectItem(root, "interval_seconds");
    if (cJSON_IsBool(enabled)) next.enabled = cJSON_IsTrue(enabled);
    if (cJSON_IsString(url)) next.url = url->valuestring;
    if (cJSON_IsString(secret_header)) next.secret_header = secret_header->valuestring;
    if (cJSON_IsString(secret_value) && secret_value->valuestring[0] != '\0') next.secret_value = secret_value->valuestring;
    if (cJSON_IsNumber(interval_seconds)) next.interval_seconds = interval_seconds->valueint;
    if (next.secret_header.empty()) next.secret_header = "x-ingest-secret";
    cJSON_Delete(root);

    esp_err_t err = cloud_client().save_webhook_config(next);
    if (err != ESP_OK) {
        return send_error(req, 400, "Invalid webhook configuration");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"Webhook settings saved\"}");
}

esp_err_t webhook_test_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    esp_err_t err = cloud_client().queue_webhook_send();
    if (err != ESP_OK) {
        return send_error(req, err == ESP_ERR_INVALID_STATE ? 400 : 500, esp_err_to_name(err));
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "Webhook send queued");
    cJSON_AddStringToObject(root, "status", "queued");
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t send_err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return send_err;
}

esp_err_t ble_get_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    BleConfig config = ble_manager().config();
    BleStatus status = ble_manager().status();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "enabled", config.enabled);
    cJSON_AddStringToObject(root, "device_name", status.device_name.c_str());
    cJSON_AddStringToObject(root, "state", status.state.c_str());
    cJSON_AddStringToObject(root, "provisioning_state", status.provisioning_state.c_str());
    cJSON_AddBoolToObject(root, "connected", status.connected);
    cJSON_AddStringToObject(root, "pairing_pin", config.pairing_pin.c_str());
    cJSON_AddBoolToObject(root, "pop_required", !config.pop_token.empty());
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t ble_post_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }

    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }

    BleConfig next = ble_manager().config();
    cJSON* enabled = cJSON_GetObjectItem(root, "enabled");
    cJSON* pairing_pin = cJSON_GetObjectItem(root, "pairing_pin");
    cJSON* pop_token = cJSON_GetObjectItem(root, "pop_token");
    if (cJSON_IsBool(enabled)) next.enabled = cJSON_IsTrue(enabled);
    if (cJSON_IsString(pairing_pin) && pairing_pin->valuestring[0] != '\0') next.pairing_pin = pairing_pin->valuestring;
    if (cJSON_IsString(pop_token)) next.pop_token = pop_token->valuestring;
    cJSON_Delete(root);

    esp_err_t err = ble_manager().apply_config(next);
    if (err != ESP_OK) {
        return send_error(req, 400, "Invalid BLE configuration");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"BLE settings saved\"}");
}

esp_err_t hardware_get_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    cJSON* root = board_support().to_json();
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t hardware_display_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }
    cJSON* enabled = cJSON_GetObjectItem(root, "enabled");
    cJSON* backlight = cJSON_GetObjectItem(root, "backlight");
    esp_err_t err = ESP_OK;
    if (cJSON_IsBool(enabled)) {
        bool on = cJSON_IsTrue(enabled);
        err = board_support().set_display_enabled(on);
        if (err == ESP_OK) {
            err = tft_display().set_enabled(on);
        }
    }
    if (err == ESP_OK && cJSON_IsBool(backlight)) {
        err = board_support().set_display_backlight(cJSON_IsTrue(backlight));
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, 500, esp_err_to_name(err));
    }
    return send_json(req, "{\"ok\":true,\"message\":\"Display settings applied\"}");
}

esp_err_t hardware_beep_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    std::string kind = "default";
    if (root) {
        cJSON* item = cJSON_GetObjectItem(root, "kind");
        if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
            kind = item->valuestring;
        }
        cJSON_Delete(root);
    }
    esp_err_t err = board_support().play_beep(kind);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json(req, "{\"ok\":true,\"message\":\"Audio request logged; I2S output is not enabled yet\"}");
    }
    if (err != ESP_OK) {
        return send_error(req, 500, esp_err_to_name(err));
    }
    return send_json(req, "{\"ok\":true,\"message\":\"Beep played\"}");
}

esp_err_t ble_sensors_scan_get_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "scanning", ble_sensor_gateway().scanning());
    cJSON_AddNumberToObject(root, "discovered_count", static_cast<double>(ble_sensor_gateway().discovered_count()));
    cJSON_AddNumberToObject(root, "paired_count", static_cast<double>(ble_sensor_gateway().paired_count()));
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t ble_sensors_scan_start_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    tiger_log("INFO", TAG, "BLE scan start API hit");
    esp_err_t err = ble_sensor_gateway().request_scan();
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            return send_json(req, "{\"ok\":true,\"message\":\"BLE host is warming up; scan will retry automatically\"}");
        }
        return send_error(req, 500, "Failed to start BLE sensor scan");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"BLE sensor scan queued\"}");
}

esp_err_t ble_sensors_scan_stop_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    esp_err_t err = ble_sensor_gateway().stop_scan();
    if (err != ESP_OK) {
        return send_error(req, 500, "Failed to stop BLE sensor scan");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"BLE sensor scan stopped\"}");
}

esp_err_t ble_sensors_discovered_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* items = cJSON_AddArrayToObject(root, "sensors");
    for (const auto& reading : ble_sensor_gateway().discovered()) {
        cJSON* item = cJSON_CreateObject();
        add_ble_sensor_json(item, reading, true);
        cJSON_AddItemToArray(items, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t ble_sensors_paired_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* items = cJSON_AddArrayToObject(root, "sensors");
    for (const auto& reading : ble_sensor_gateway().paired_latest()) {
        cJSON* item = cJSON_CreateObject();
        add_ble_sensor_json(item, reading, true);
        cJSON_AddItemToArray(items, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t ble_sensors_latest_handler(httpd_req_t* req) {
    return ble_sensors_paired_handler(req);
}

esp_err_t ble_sensors_pair_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }
    cJSON* mac = cJSON_GetObjectItem(root, "mac");
    cJSON* name = cJSON_GetObjectItem(root, "name");
    cJSON* type = cJSON_GetObjectItem(root, "sensor_type");
    cJSON* brand = cJSON_GetObjectItem(root, "brand");
    cJSON* model = cJSON_GetObjectItem(root, "model");
    cJSON* protocol = cJSON_GetObjectItem(root, "protocol");
    cJSON* location = cJSON_GetObjectItem(root, "location");
    if (!cJSON_IsString(mac) || mac->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_error(req, 400, "Sensor MAC is required");
    }
    PairedBleSensorConfig config;
    config.mac_address = mac->valuestring;
    config.name = cJSON_IsString(name) ? name->valuestring : mac->valuestring;
    config.brand = cJSON_IsString(brand) ? brand->valuestring : "unknown";
    config.model = cJSON_IsString(model) ? model->valuestring : "unknown";
    config.protocol = cJSON_IsString(protocol) ? protocol->valuestring : (cJSON_IsString(type) ? type->valuestring : "unknown");
    config.location = cJSON_IsString(location) ? location->valuestring : "";
    esp_err_t err = ble_sensor_gateway().pair_sensor(config);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            return send_error(req, 400, "Failed to watch BLE sensor: watched list is full or MAC is invalid");
        }
        if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE || err == ESP_ERR_NVS_VALUE_TOO_LONG || err == ESP_ERR_NO_MEM) {
            return send_error(req, 500, "Failed to watch BLE sensor: NVS storage is full");
        }
        return send_error(req, 500, esp_err_to_name(err));
    }
    return send_json(req, "{\"ok\":true,\"message\":\"BLE sensor watched\"}");
}

esp_err_t ble_sensors_remove_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }
    cJSON* mac = cJSON_GetObjectItem(root, "mac");
    if (!cJSON_IsString(mac) || mac->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_error(req, 400, "Sensor MAC is required");
    }
    esp_err_t err = ble_sensor_gateway().remove_sensor(mac->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, 400, "Failed to remove BLE sensor");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"BLE sensor removed\"}");
}

esp_err_t ble_sensors_rename_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }
    cJSON* mac = cJSON_GetObjectItem(root, "mac");
    cJSON* name = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(mac) || mac->valuestring[0] == '\0' || !cJSON_IsString(name) || name->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_error(req, 400, "Sensor MAC and name are required");
    }
    esp_err_t err = ble_sensor_gateway().rename_sensor(mac->valuestring, name->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, 400, "Failed to rename BLE sensor");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"BLE sensor renamed\"}");
}

esp_err_t ble_sensors_location_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }
    cJSON* mac = cJSON_GetObjectItem(root, "mac");
    cJSON* location = cJSON_GetObjectItem(root, "location");
    if (!cJSON_IsString(mac) || mac->valuestring[0] == '\0' || !cJSON_IsString(location)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Sensor MAC and location are required");
    }
    esp_err_t err = ble_sensor_gateway().set_location(mac->valuestring, location->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, 400, "Failed to update BLE sensor location");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"BLE sensor location updated\"}");
}

esp_err_t ble_sensors_bindkey_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }
    cJSON* mac = cJSON_GetObjectItem(root, "mac");
    cJSON* bindkey = cJSON_GetObjectItem(root, "bindkey");
    if (!cJSON_IsString(mac) || mac->valuestring[0] == '\0' || !cJSON_IsString(bindkey)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Sensor MAC and bindkey are required");
    }
    esp_err_t err = ble_sensor_gateway().set_bindkey(mac->valuestring, bindkey->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, 400, "Bindkey must be blank or 32 hex characters");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"BLE sensor bindkey saved\"}");
}

esp_err_t ble_sensors_raw_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    constexpr size_t MAX_RAW_API_PACKETS = 30;
    constexpr size_t MAX_UNKNOWN_RAW_API_PACKETS = 8;
    const auto packets = ble_sensor_gateway().raw_packet_history();
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return send_error(req, 500, "Not enough memory to create BLE raw response");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "total", static_cast<double>(packets.size()));
    cJSON* items = cJSON_AddArrayToObject(root, "packets");
    size_t emitted = 0;
    size_t unknown_emitted = 0;
    for (const auto& packet : packets) {
        const bool unknown_sample = packet.protocol == "unknown" && packet.service_data_hex.empty() && !ble_raw_packet_has_values(packet);
        const bool useful_sample = ble_raw_packet_has_values(packet) || packet.protocol == "bthome" ||
                                   packet.protocol == "inkbird" || packet.protocol == "atc" ||
                                   packet.protocol == "pvvx" || !packet.service_data_hex.empty();
        if (!useful_sample && !unknown_sample) {
            continue;
        }
        if (unknown_sample && unknown_emitted >= MAX_UNKNOWN_RAW_API_PACKETS) {
            continue;
        }
        if (emitted >= MAX_RAW_API_PACKETS) {
            break;
        }
        cJSON* item = cJSON_CreateObject();
        if (!item) {
            break;
        }
        add_ble_raw_packet_json(item, packet);
        cJSON_AddItemToArray(items, item);
        emitted++;
        if (unknown_sample) {
            unknown_emitted++;
        }
    }
    cJSON_AddNumberToObject(root, "returned", static_cast<double>(emitted));
    char* text = cJSON_PrintUnformatted(root);
    if (!text) {
        cJSON_Delete(root);
        return send_error(req, 500, "Not enough memory to serialize BLE raw response");
    }
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t ble_raw_handler(httpd_req_t* req) {
    return ble_sensors_raw_handler(req);
}

esp_err_t ble_stats_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    BleScannerStats stats = ble_sensor_gateway().stats();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "scanning", stats.scanning);
    cJSON_AddBoolToObject(root, "scan_requested", stats.scan_requested);
    cJSON_AddBoolToObject(root, "provisioning_advertising", stats.provisioning_advertising);
    cJSON_AddNumberToObject(root, "raw_packet_count", static_cast<double>(stats.raw_packet_count));
    cJSON_AddNumberToObject(root, "discovered_device_count", static_cast<double>(stats.discovered_device_count));
    cJSON_AddNumberToObject(root, "paired_device_count", static_cast<double>(stats.paired_device_count));
    cJSON_AddNumberToObject(root, "readable_device_count", static_cast<double>(stats.readable_device_count));
    cJSON_AddNumberToObject(root, "bthome_packet_count", static_cast<double>(stats.bthome_packet_count));
    cJSON_AddNumberToObject(root, "unknown_packet_count", static_cast<double>(stats.unknown_packet_count));
    cJSON_AddNumberToObject(root, "last_scan_started", static_cast<double>(stats.last_scan_started));
    cJSON_AddNumberToObject(root, "last_packet_seen", static_cast<double>(stats.last_packet_seen));
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t ble_discovered_handler(httpd_req_t* req) {
    return ble_sensors_discovered_handler(req);
}

esp_err_t ble_raw_clear_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    ble_sensor_gateway().clear_raw_packets();
    return send_json(req, "{\"ok\":true,\"message\":\"BLE raw packets cleared\"}");
}

esp_err_t ble_gatt_inspect_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return send_error(req, 400, "Invalid JSON body");
    }
    cJSON* mac = cJSON_GetObjectItem(root, "mac");
    if (!cJSON_IsString(mac) || mac->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_error(req, 400, "BLE MAC is required");
    }
    esp_err_t err = ble_sensor_gateway().inspect_gatt(mac->valuestring);
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, 409, "BLE GATT inspector is busy or not ready");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, 400, "Invalid BLE MAC address");
    }
    if (err != ESP_OK) {
        return send_error(req, 500, "Failed to start BLE GATT inspection");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"BLE GATT inspection started\"}");
}

esp_err_t ble_gatt_inspection_handler(httpd_req_t* req) {
    if (!authorized(req)) {
        return require_auth(req);
    }
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return send_error(req, 500, "Out of memory");
    }
    add_ble_gatt_inspection_json(root, ble_sensor_gateway().gatt_inspection());
    char* text = cJSON_PrintUnformatted(root);
    if (!text) {
        cJSON_Delete(root);
        return send_error(req, 500, "Out of memory");
    }
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

std::string wildcard_tail(httpd_req_t* req, const char* prefix) {
    std::string uri = req->uri;
    std::string base = prefix;
    if (uri.rfind(base, 0) != 0) return {};
    std::string tail = uri.substr(base.size());
    while (!tail.empty() && tail.front() == '/') tail.erase(tail.begin());
    return tail;
}

esp_err_t devices_list_handler(httpd_req_t* req) {
    if (!authorized(req)) return require_auth(req);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* items = cJSON_AddArrayToObject(root, "devices");
    for (const auto& device : device_registry().devices()) {
        cJSON_AddItemToArray(items, universal_device_to_json(device, false));
    }
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t devices_discovered_handler(httpd_req_t* req) {
    if (!authorized(req)) return require_auth(req);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* items = cJSON_AddArrayToObject(root, "devices");
    for (const auto& device : device_registry().discovered()) {
        cJSON_AddItemToArray(items, universal_device_to_json(device, true));
    }
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t devices_scan_handler(httpd_req_t* req) {
    if (!authorized(req)) return require_auth(req);
    std::string body = read_body(req);
    std::string method = "ble";
    cJSON* root = cJSON_Parse(body.c_str());
    if (root) {
        cJSON* item = cJSON_GetObjectItem(root, "method");
        if (cJSON_IsString(item)) method = item->valuestring;
        cJSON_Delete(root);
    }
    esp_err_t err = device_registry().scan(method);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            return send_json(req, "{\"ok\":true,\"message\":\"BLE host is warming up; scan will retry automatically\"}");
        }
        return send_error(req, 500, "Device scan failed");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"Device scan started\"}");
}

esp_err_t devices_pair_handler(httpd_req_t* req) {
    if (!authorized(req)) return require_auth(req);
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return send_error(req, 400, "Invalid JSON body");
    esp_err_t err = device_registry().pair(root);
    cJSON_Delete(root);
    if (err != ESP_OK) return send_error(req, 400, "Device watch failed");
    return send_json(req, "{\"ok\":true,\"message\":\"Device watched\"}");
}

esp_err_t devices_remove_handler(httpd_req_t* req) {
    if (!authorized(req)) return require_auth(req);
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return send_error(req, 400, "Invalid JSON body");
    cJSON* id = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsString(id)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Device id is required");
    }
    esp_err_t err = device_registry().remove(id->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) return send_error(req, 400, "Device remove failed");
    return send_json(req, "{\"ok\":true,\"message\":\"Device removed\"}");
}

esp_err_t devices_rename_handler(httpd_req_t* req) {
    if (!authorized(req)) return require_auth(req);
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return send_error(req, 400, "Invalid JSON body");
    cJSON* id = cJSON_GetObjectItem(root, "id");
    cJSON* name = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(id) || !cJSON_IsString(name)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Device id and name are required");
    }
    esp_err_t err = device_registry().rename(id->valuestring, name->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) return send_error(req, 400, "Device rename failed");
    return send_json(req, "{\"ok\":true,\"message\":\"Device renamed\"}");
}

esp_err_t devices_location_handler(httpd_req_t* req) {
    if (!authorized(req)) return require_auth(req);
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return send_error(req, 400, "Invalid JSON body");
    cJSON* id = cJSON_GetObjectItem(root, "id");
    cJSON* location = cJSON_GetObjectItem(root, "location");
    if (!cJSON_IsString(id) || !cJSON_IsString(location)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Device id and location are required");
    }
    esp_err_t err = device_registry().set_location(id->valuestring, location->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) return send_error(req, 400, "Device location update failed");
    return send_json(req, "{\"ok\":true,\"message\":\"Device location updated\"}");
}

esp_err_t devices_get_wildcard_handler(httpd_req_t* req) {
    if (!authorized(req)) return require_auth(req);
    std::string tail = wildcard_tail(req, "/api/devices");
    if (tail.empty()) return devices_list_handler(req);
    bool state_only = false;
    bool raw = false;
    if (tail.size() > 6 && tail.rfind("/state") == tail.size() - 6) {
        tail = tail.substr(0, tail.size() - 6);
        state_only = true;
    } else if (tail.size() > 4 && tail.rfind("/raw") == tail.size() - 4) {
        tail = tail.substr(0, tail.size() - 4);
        raw = true;
    }
    auto device = device_registry().get(tail);
    if (!device) return send_error(req, 404, "Device not found");
    if (state_only) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON* state = cJSON_Parse(device->state_json.c_str());
        cJSON_AddItemToObject(root, "state", state ? state : cJSON_CreateObject());
        char* text = cJSON_PrintUnformatted(root);
        esp_err_t err = send_json(req, text);
        cJSON_free(text);
        cJSON_Delete(root);
        return err;
    }
    cJSON* root = universal_device_to_json(*device, raw);
    cJSON_AddBoolToObject(root, "ok", true);
    char* text = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t devices_control_wildcard_handler(httpd_req_t* req) {
    if (!authorized(req)) return require_auth(req);
    std::string tail = wildcard_tail(req, "/api/devices");
    if (tail.size() <= 8 || tail.rfind("/control") != tail.size() - 8) {
        return send_error(req, 404, "Device control route not found");
    }
    std::string id = tail.substr(0, tail.size() - 8);
    std::string body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return send_error(req, 400, "Invalid JSON body");
    cJSON* capability = cJSON_GetObjectItem(root, "capability");
    cJSON* value = cJSON_GetObjectItem(root, "value");
    if (!cJSON_IsString(capability)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Capability is required");
    }
    esp_err_t err = device_registry().control(id, capability->valuestring, value);
    cJSON_Delete(root);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, 400, "Device capability is not controllable");
    if (err != ESP_OK) return send_error(req, 400, "Device control failed");
    return send_json(req, "{\"ok\":true,\"message\":\"Device control accepted\"}");
}

httpd_uri_t uri(const char* path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t out = {};
    out.uri = path;
    out.method = method;
    out.handler = handler;
    out.user_ctx = nullptr;
    return out;
}

} // namespace

esp_err_t register_api_routes(httpd_handle_t server) {
    httpd_uri_t status = uri("/api/status", HTTP_GET, status_handler);
    httpd_uri_t login = uri("/api/login", HTTP_POST, login_handler);
    httpd_uri_t wifi = uri("/api/wifi", HTTP_POST, wifi_handler);
    httpd_uri_t wifi_scan = uri("/api/wifi/scan", HTTP_GET, wifi_scan_handler);
    httpd_uri_t wifi_forget = uri("/api/wifi/forget", HTTP_POST, wifi_forget_handler);
    httpd_uri_t reboot = uri("/api/reboot", HTTP_POST, reboot_handler);
    httpd_uri_t factory_reset = uri("/api/factory-reset", HTTP_POST, factory_reset_handler);
    httpd_uri_t ota = uri("/api/ota", HTTP_POST, ota_handler);
    httpd_uri_t logs = uri("/api/logs", HTTP_GET, logs_handler);
    httpd_uri_t logs_clear = uri("/api/logs/clear", HTTP_POST, logs_clear_handler);
    httpd_uri_t control = uri("/api/control", HTTP_POST, control_handler);
    httpd_uri_t ota_check = uri("/api/ota/check", HTTP_POST, ota_check_handler);
    httpd_uri_t ota_config_get = uri("/api/ota/config", HTTP_GET, ota_config_get_handler);
    httpd_uri_t ota_config_post = uri("/api/ota/config", HTTP_POST, ota_config_post_handler);
    httpd_uri_t ota_update = uri("/api/ota/update", HTTP_POST, ota_update_handler);
    httpd_uri_t mqtt_get = uri("/api/mqtt", HTTP_GET, mqtt_get_handler);
    httpd_uri_t mqtt_post = uri("/api/mqtt", HTTP_POST, mqtt_post_handler);
    httpd_uri_t webhook_get = uri("/api/webhook", HTTP_GET, webhook_get_handler);
    httpd_uri_t webhook_post = uri("/api/webhook", HTTP_POST, webhook_post_handler);
    httpd_uri_t webhook_test = uri("/api/webhook/test", HTTP_POST, webhook_test_handler);
    httpd_uri_t webhook_sync = uri("/api/webhook/sync", HTTP_POST, webhook_test_handler);
    httpd_uri_t ble_get = uri("/api/ble", HTTP_GET, ble_get_handler);
    httpd_uri_t ble_post = uri("/api/ble", HTTP_POST, ble_post_handler);
    httpd_uri_t hardware_get = uri("/api/hardware", HTTP_GET, hardware_get_handler);
    httpd_uri_t hardware_display = uri("/api/hardware/display", HTTP_POST, hardware_display_handler);
    httpd_uri_t hardware_beep = uri("/api/hardware/beep", HTTP_POST, hardware_beep_handler);
    httpd_uri_t ble_sensor_scan_start = uri("/api/ble-sensors/scan/start", HTTP_POST, ble_sensors_scan_start_handler);
    httpd_uri_t ble_sensor_scan_stop = uri("/api/ble-sensors/scan/stop", HTTP_POST, ble_sensors_scan_stop_handler);
    httpd_uri_t ble_sensor_scan_status = uri("/api/ble-sensors/scan/status", HTTP_GET, ble_sensors_scan_get_handler);
    httpd_uri_t ble_sensor_scan_get = uri("/api/ble-sensors/scan", HTTP_GET, ble_sensors_scan_get_handler);
    httpd_uri_t ble_sensor_discovered = uri("/api/ble-sensors/discovered", HTTP_GET, ble_sensors_discovered_handler);
    httpd_uri_t ble_sensor_paired = uri("/api/ble-sensors/paired", HTTP_GET, ble_sensors_paired_handler);
    httpd_uri_t ble_sensor_pair = uri("/api/ble-sensors/pair", HTTP_POST, ble_sensors_pair_handler);
    httpd_uri_t ble_sensor_remove = uri("/api/ble-sensors/remove", HTTP_POST, ble_sensors_remove_handler);
    httpd_uri_t ble_sensor_rename = uri("/api/ble-sensors/rename", HTTP_POST, ble_sensors_rename_handler);
    httpd_uri_t ble_sensor_location = uri("/api/ble-sensors/location", HTTP_POST, ble_sensors_location_handler);
    httpd_uri_t ble_sensor_bindkey = uri("/api/ble-sensors/bindkey", HTTP_POST, ble_sensors_bindkey_handler);
    httpd_uri_t ble_sensor_latest = uri("/api/ble-sensors/latest", HTTP_GET, ble_sensors_latest_handler);
    httpd_uri_t ble_sensor_raw = uri("/api/ble-sensors/raw", HTTP_GET, ble_sensors_raw_handler);
    httpd_uri_t ble_raw = uri("/api/ble/raw", HTTP_GET, ble_raw_handler);
    httpd_uri_t ble_stats = uri("/api/ble/stats", HTTP_GET, ble_stats_handler);
    httpd_uri_t ble_discovered = uri("/api/ble/discovered", HTTP_GET, ble_discovered_handler);
    httpd_uri_t ble_raw_clear = uri("/api/ble/raw/clear", HTTP_POST, ble_raw_clear_handler);
    httpd_uri_t ble_gatt_inspect = uri("/api/ble/gatt/inspect", HTTP_POST, ble_gatt_inspect_handler);
    httpd_uri_t ble_gatt_inspection = uri("/api/ble/gatt/inspection", HTTP_GET, ble_gatt_inspection_handler);
    httpd_uri_t devices_list = uri("/api/devices", HTTP_GET, devices_list_handler);
    httpd_uri_t devices_discovered = uri("/api/devices/discovered", HTTP_GET, devices_discovered_handler);
    httpd_uri_t devices_scan = uri("/api/devices/scan", HTTP_POST, devices_scan_handler);
    httpd_uri_t devices_pair = uri("/api/devices/pair", HTTP_POST, devices_pair_handler);
    httpd_uri_t devices_remove = uri("/api/devices/remove", HTTP_POST, devices_remove_handler);
    httpd_uri_t devices_rename = uri("/api/devices/rename", HTTP_POST, devices_rename_handler);
    httpd_uri_t devices_location = uri("/api/devices/location", HTTP_POST, devices_location_handler);
    httpd_uri_t devices_get_wildcard = uri("/api/devices/*", HTTP_GET, devices_get_wildcard_handler);
    httpd_uri_t devices_control_wildcard = uri("/api/devices/*", HTTP_POST, devices_control_wildcard_handler);

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &login));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_scan));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_forget));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reboot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &factory_reset));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &logs));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &logs_clear));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &control));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_check));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_config_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_config_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_update));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mqtt_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mqtt_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &webhook_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &webhook_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &webhook_test));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &webhook_sync));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &hardware_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &hardware_display));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &hardware_beep));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_scan_start));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_scan_stop));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_scan_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_scan_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_discovered));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_paired));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_pair));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_remove));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_rename));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_location));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_bindkey));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_latest));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_sensor_raw));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_raw));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_stats));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_discovered));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_raw_clear));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_gatt_inspect));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_gatt_inspection));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_list));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_discovered));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_scan));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_pair));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_remove));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_rename));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_location));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_get_wildcard));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_control_wildcard));
    return ESP_OK;
}

} // namespace tigeros
