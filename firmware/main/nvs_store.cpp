#include "nvs_store.h"

#include <array>
#include <cstdio>
#include <inttypes.h>

#include "esp_random.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "nvs_store";
constexpr const char* NAMESPACE = "tigeros";
constexpr const char* KEY_WIFI_SSID = "wifi_ssid";
constexpr const char* KEY_WIFI_PASS = "wifi_pass";
constexpr const char* KEY_DEVICE_ID = "device_id";
constexpr const char* KEY_FW_VERSION = "fw_version";
constexpr const char* KEY_CLOUD_TOKEN = "cloud_token";
constexpr const char* KEY_API_TOKEN = "api_token";
constexpr const char* KEY_PASS_HASH = "pass_hash";
constexpr const char* KEY_OTA_ENABLED = "ota_enabled";
constexpr const char* KEY_OTA_AUTO_UPDATE = "ota_auto";
constexpr const char* KEY_OTA_CHECK_URL = "ota_url";
constexpr const char* KEY_OTA_CHANNEL = "ota_channel";
constexpr const char* KEY_MQTT_ENABLED = "mqtt_enabled";
constexpr const char* KEY_MQTT_HOST = "mqtt_host";
constexpr const char* KEY_MQTT_PORT = "mqtt_port";
constexpr const char* KEY_MQTT_USERNAME = "mqtt_user";
constexpr const char* KEY_MQTT_PASSWORD = "mqtt_pass";
constexpr const char* KEY_MQTT_CLIENT_ID = "mqtt_client";
constexpr const char* KEY_MQTT_USE_TLS = "mqtt_tls";
constexpr const char* KEY_HA_DISCOVERY = "ha_discovery";
constexpr const char* KEY_HA_PREFIX = "ha_prefix";
constexpr const char* KEY_BLE_ENABLED = "ble_enabled";
constexpr const char* KEY_BLE_PIN = "ble_pin";
constexpr const char* KEY_BLE_POP = "ble_pop";
constexpr const char* KEY_WEBHOOK_ENABLED = "wh_enabled";
constexpr const char* KEY_WEBHOOK_URL = "wh_url";
constexpr const char* KEY_WEBHOOK_HEADER = "wh_header";
constexpr const char* KEY_WEBHOOK_SECRET = "wh_secret";
constexpr const char* KEY_WEBHOOK_INTERVAL = "wh_interval";
constexpr const char* KEY_BLE_SENSOR_PAIRS = "ble_sensor_pairs";
constexpr const char* KEY_BLE_SENSOR_LAST = "ble_sensor_last";
constexpr const char* KEY_UNIVERSAL_DEVICES = "uni_devices";

bool json_bool(cJSON* item, const char* key) {
    cJSON* value = cJSON_GetObjectItem(item, key);
    return cJSON_IsBool(value) && cJSON_IsTrue(value);
}

double json_number(cJSON* item, const char* key, double fallback = 0) {
    cJSON* value = cJSON_GetObjectItem(item, key);
    return cJSON_IsNumber(value) ? value->valuedouble : fallback;
}

std::string json_string(cJSON* item, const char* key, const char* fallback = "") {
    cJSON* value = cJSON_GetObjectItem(item, key);
    return cJSON_IsString(value) ? value->valuestring : fallback;
}

bool starts_with(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool valid_http_url(const std::string& url) {
    return starts_with(url, "https://") || starts_with(url, "http://");
}

bool valid_header_name(const std::string& name) {
    if (name.empty() || name.size() > 48) {
        return false;
    }
    for (unsigned char ch : name) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

esp_err_t NvsStore::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

std::optional<WifiCredentials> NvsStore::get_wifi_credentials() {
    std::string ssid;
    std::string password;
    if (get_string(KEY_WIFI_SSID, ssid) != ESP_OK || ssid.empty()) {
        return std::nullopt;
    }
    if (get_string(KEY_WIFI_PASS, password) != ESP_OK) {
        password.clear();
    }
    return WifiCredentials{ssid, password};
}

esp_err_t NvsStore::save_wifi_credentials(const std::string& ssid, const std::string& password) {
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, KEY_WIFI_SSID, ssid.c_str());
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_WIFI_PASS, password.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t NvsStore::clear_wifi_credentials() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t ssid_err = nvs_erase_key(handle, KEY_WIFI_SSID);
    esp_err_t pass_err = nvs_erase_key(handle, KEY_WIFI_PASS);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (ssid_err != ESP_OK && ssid_err != ESP_ERR_NVS_NOT_FOUND) {
        return ssid_err;
    }
    if (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND) {
        return pass_err;
    }
    return err;
}

esp_err_t NvsStore::factory_reset_config() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    const char* keys[] = {
        KEY_WIFI_SSID,
        KEY_WIFI_PASS,
        KEY_CLOUD_TOKEN,
        KEY_API_TOKEN,
        KEY_PASS_HASH,
        KEY_OTA_ENABLED,
        KEY_OTA_AUTO_UPDATE,
        KEY_OTA_CHECK_URL,
        KEY_OTA_CHANNEL,
        KEY_MQTT_ENABLED,
        KEY_MQTT_HOST,
        KEY_MQTT_PORT,
        KEY_MQTT_USERNAME,
        KEY_MQTT_PASSWORD,
        KEY_MQTT_CLIENT_ID,
        KEY_MQTT_USE_TLS,
        KEY_HA_DISCOVERY,
        KEY_HA_PREFIX,
        KEY_BLE_ENABLED,
        KEY_BLE_PIN,
        KEY_BLE_POP,
        KEY_WEBHOOK_ENABLED,
        KEY_WEBHOOK_URL,
        KEY_WEBHOOK_HEADER,
        KEY_WEBHOOK_SECRET,
        KEY_WEBHOOK_INTERVAL,
        KEY_BLE_SENSOR_PAIRS,
        KEY_BLE_SENSOR_LAST,
        KEY_UNIVERSAL_DEVICES,
    };

    for (const char* key : keys) {
        esp_err_t erase_err = nvs_erase_key(handle, key);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND && err == ESP_OK) {
            err = erase_err;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

std::string NvsStore::get_or_create_device_id() {
    std::string value;
    if (get_string(KEY_DEVICE_ID, value) == ESP_OK && !value.empty()) {
        return value;
    }

    std::array<uint8_t, 6> mac{};
    ESP_ERROR_CHECK(esp_read_mac(mac.data(), ESP_MAC_WIFI_STA));
    char id[24];
    std::snprintf(id, sizeof(id), "tiger-%02X%02X%02X%02X%02X%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    value = id;
    esp_err_t err = set_string(KEY_DEVICE_ID, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist device id: %s", esp_err_to_name(err));
    }
    return value;
}

std::string NvsStore::get_firmware_version() {
    std::string value;
    if (get_string(KEY_FW_VERSION, value) == ESP_OK && !value.empty()) {
        return value;
    }
    return "unknown";
}

esp_err_t NvsStore::ensure_firmware_version(const std::string& version) {
    return set_string(KEY_FW_VERSION, version);
}

std::string NvsStore::get_cloud_token() {
    std::string value;
    if (get_string(KEY_CLOUD_TOKEN, value) == ESP_OK) {
        return value;
    }
    return {};
}

esp_err_t NvsStore::save_cloud_token(const std::string& token) {
    return set_string(KEY_CLOUD_TOKEN, token);
}

esp_err_t NvsStore::clear_cloud_token() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, KEY_CLOUD_TOKEN);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

std::string NvsStore::get_or_create_api_token() {
    std::string value;
    if (get_string(KEY_API_TOKEN, value) == ESP_OK && value.size() >= 32) {
        return value;
    }

    char token[65];
    for (size_t i = 0; i < 64; i += 8) {
        std::snprintf(token + i, 9, "%08" PRIx32, esp_random());
    }
    token[64] = '\0';
    value = token;
    esp_err_t err = set_string(KEY_API_TOKEN, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist API token: %s", esp_err_to_name(err));
    }
    return value;
}

std::string NvsStore::get_password_hash() {
    std::string value;
    if (get_string(KEY_PASS_HASH, value) == ESP_OK) {
        return value;
    }
    return {};
}

esp_err_t NvsStore::save_password_hash(const std::string& hash) {
    return set_string(KEY_PASS_HASH, hash);
}

std::string NvsStore::get_ota_check_url() {
    std::string value;
    if (get_string(KEY_OTA_CHECK_URL, value) == ESP_OK) {
        return value;
    }
    return {};
}

esp_err_t NvsStore::save_ota_check_url(const std::string& url) {
    return set_string(KEY_OTA_CHECK_URL, url);
}

std::string NvsStore::get_ota_channel() {
    std::string value;
    if (get_string(KEY_OTA_CHANNEL, value) == ESP_OK && !value.empty()) {
        return value;
    }
    return "stable";
}

esp_err_t NvsStore::save_ota_channel(const std::string& channel) {
    return set_string(KEY_OTA_CHANNEL, channel);
}

OtaConfig NvsStore::get_ota_config() {
    OtaConfig config;
    int32_t value = 0;
    if (get_i32(KEY_OTA_ENABLED, value) == ESP_OK) {
        config.enabled = value != 0;
    }
    if (get_i32(KEY_OTA_AUTO_UPDATE, value) == ESP_OK) {
        config.auto_update = value != 0;
    }
    get_string(KEY_OTA_CHECK_URL, config.check_url);
    config.channel = get_ota_channel();
    if (config.channel != "stable" && config.channel != "beta") {
        config.channel = "stable";
    }
    return config;
}

esp_err_t NvsStore::save_ota_config(const OtaConfig& config) {
    if (config.enabled && config.check_url.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config.channel != "stable" && config.channel != "beta") {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(handle, KEY_OTA_ENABLED, config.enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(handle, KEY_OTA_AUTO_UPDATE, config.auto_update ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_OTA_CHECK_URL, config.check_url.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_OTA_CHANNEL, config.channel.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

MqttConfig NvsStore::get_mqtt_config() {
    MqttConfig config;
    int32_t value = 0;
    if (get_i32(KEY_MQTT_ENABLED, value) == ESP_OK) {
        config.enabled = value != 0;
    }
    if (get_i32(KEY_MQTT_PORT, value) == ESP_OK && value > 0) {
        config.port = value;
    }
    if (get_i32(KEY_MQTT_USE_TLS, value) == ESP_OK) {
        config.use_tls = value != 0;
    }
    value = 1;
    if (get_i32(KEY_HA_DISCOVERY, value) == ESP_OK) {
        config.ha_discovery_enabled = value != 0;
    }
    get_string(KEY_MQTT_HOST, config.host);
    get_string(KEY_MQTT_USERNAME, config.username);
    get_string(KEY_MQTT_PASSWORD, config.password);
    get_string(KEY_MQTT_CLIENT_ID, config.client_id);
    get_string(KEY_HA_PREFIX, config.ha_discovery_prefix);
    if (config.client_id.empty()) {
        config.client_id = get_or_create_device_id();
    }
    if (config.ha_discovery_prefix.empty()) {
        config.ha_discovery_prefix = "homeassistant";
    }
    if (config.port <= 0) {
        config.port = config.use_tls ? 8883 : 1883;
    }
    return config;
}

esp_err_t NvsStore::save_mqtt_config(const MqttConfig& config) {
    if (config.enabled && config.host.empty()) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(handle, KEY_MQTT_ENABLED, config.enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_MQTT_HOST, config.host.c_str());
    if (err == ESP_OK) err = nvs_set_i32(handle, KEY_MQTT_PORT, config.port);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_MQTT_USERNAME, config.username.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_MQTT_PASSWORD, config.password.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_MQTT_CLIENT_ID, config.client_id.c_str());
    if (err == ESP_OK) err = nvs_set_i32(handle, KEY_MQTT_USE_TLS, config.use_tls ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(handle, KEY_HA_DISCOVERY, config.ha_discovery_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_HA_PREFIX, config.ha_discovery_prefix.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

BleConfig NvsStore::get_ble_config() {
    BleConfig config;
    int32_t value = 1;
    if (get_i32(KEY_BLE_ENABLED, value) == ESP_OK) {
        config.enabled = value != 0;
    }
    std::string pin;
    if (get_string(KEY_BLE_PIN, pin) == ESP_OK && pin.size() == 6) {
        config.pairing_pin = pin;
    }
    get_string(KEY_BLE_POP, config.pop_token);
    return config;
}

esp_err_t NvsStore::save_ble_config(const BleConfig& config) {
    if (!config.pairing_pin.empty() && config.pairing_pin.size() != 6) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(handle, KEY_BLE_ENABLED, config.enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_BLE_PIN, config.pairing_pin.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_BLE_POP, config.pop_token.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

WebhookConfig NvsStore::get_webhook_config() {
    WebhookConfig config;
    int32_t value = 0;
    if (get_i32(KEY_WEBHOOK_ENABLED, value) == ESP_OK) {
        config.enabled = value != 0;
    }
    if (get_i32(KEY_WEBHOOK_INTERVAL, value) == ESP_OK && value >= 10 && value <= 3600) {
        config.interval_seconds = value;
    }
    get_string(KEY_WEBHOOK_URL, config.url);
    get_string(KEY_WEBHOOK_HEADER, config.secret_header);
    get_string(KEY_WEBHOOK_SECRET, config.secret_value);
    if (config.secret_header.empty()) {
        config.secret_header = "x-ingest-secret";
    }
    return config;
}

esp_err_t NvsStore::save_webhook_config(const WebhookConfig& config) {
    if (config.enabled && (config.url.empty() || !valid_http_url(config.url))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config.url.empty() && !valid_http_url(config.url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config.secret_header.empty() && !valid_header_name(config.secret_header)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config.interval_seconds < 10 || config.interval_seconds > 3600) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(handle, KEY_WEBHOOK_ENABLED, config.enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_WEBHOOK_URL, config.url.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_WEBHOOK_HEADER, config.secret_header.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_WEBHOOK_SECRET, config.secret_value.c_str());
    if (err == ESP_OK) err = nvs_set_i32(handle, KEY_WEBHOOK_INTERVAL, config.interval_seconds);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

std::vector<PairedBleSensorConfig> NvsStore::get_ble_sensor_pairs() {
    std::vector<PairedBleSensorConfig> sensors;
    std::string json;
    if (get_string(KEY_BLE_SENSOR_PAIRS, json) != ESP_OK || json.empty()) {
        return sensors;
    }

    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return sensors;
    }

    const int count = cJSON_GetArraySize(root);
    sensors.reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        cJSON* mac = cJSON_GetObjectItem(item, "mac");
        if (!cJSON_IsString(mac) || mac->valuestring[0] == '\0') {
            continue;
        }
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* type = cJSON_GetObjectItem(item, "type");
        cJSON* brand = cJSON_GetObjectItem(item, "brand");
        cJSON* model = cJSON_GetObjectItem(item, "model");
        cJSON* protocol = cJSON_GetObjectItem(item, "protocol");
        cJSON* location = cJSON_GetObjectItem(item, "location");
        cJSON* bindkey = cJSON_GetObjectItem(item, "bindkey");
        PairedBleSensorConfig sensor;
        sensor.mac_address = mac->valuestring;
        sensor.name = cJSON_IsString(name) ? name->valuestring : sensor.mac_address;
        sensor.brand = cJSON_IsString(brand) && brand->valuestring[0] ? brand->valuestring : "unknown";
        sensor.model = cJSON_IsString(model) && model->valuestring[0] ? model->valuestring : "unknown";
        sensor.protocol = cJSON_IsString(protocol) && protocol->valuestring[0] ? protocol->valuestring : "unknown";
        if (cJSON_IsString(type) && type->valuestring[0] && sensor.protocol == "unknown") {
            sensor.protocol = type->valuestring;
        }
        sensor.location = cJSON_IsString(location) ? location->valuestring : "";
        sensor.bindkey = cJSON_IsString(bindkey) ? bindkey->valuestring : "";
        sensors.push_back(sensor);
    }
    cJSON_Delete(root);
    return sensors;
}

esp_err_t NvsStore::save_ble_sensor_pairs(const std::vector<PairedBleSensorConfig>& sensors) {
    if (sensors.size() > 32) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON* root = cJSON_CreateArray();
    for (const auto& sensor : sensors) {
        if (sensor.mac_address.empty()) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", sensor.mac_address.c_str());
        cJSON_AddStringToObject(item, "name", sensor.name.empty() ? sensor.mac_address.c_str() : sensor.name.c_str());
        cJSON_AddStringToObject(item, "brand", sensor.brand.empty() ? "unknown" : sensor.brand.c_str());
        cJSON_AddStringToObject(item, "model", sensor.model.empty() ? "unknown" : sensor.model.c_str());
        cJSON_AddStringToObject(item, "protocol", sensor.protocol.empty() ? "unknown" : sensor.protocol.c_str());
        cJSON_AddStringToObject(item, "type", sensor.protocol.empty() ? "unknown" : sensor.protocol.c_str());
        cJSON_AddStringToObject(item, "location", sensor.location.c_str());
        cJSON_AddStringToObject(item, "bindkey", sensor.bindkey.c_str());
        cJSON_AddItemToArray(root, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string value = text ? text : "[]";
    cJSON_free(text);
    cJSON_Delete(root);
    return set_string(KEY_BLE_SENSOR_PAIRS, value);
}

std::vector<BleSensorLastReading> NvsStore::get_ble_sensor_last_readings() {
    std::vector<BleSensorLastReading> readings;
    std::string json;
    if (get_string(KEY_BLE_SENSOR_LAST, json) != ESP_OK || json.empty()) {
        return readings;
    }
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return readings;
    }

    const int count = cJSON_GetArraySize(root);
    readings.reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        const std::string mac = json_string(item, "mac");
        if (mac.empty()) {
            continue;
        }
        BleSensorLastReading reading;
        reading.mac_address = mac;
        reading.name = json_string(item, "name", mac.c_str());
        reading.display_name = json_string(item, "display_name", reading.name.c_str());
        reading.brand = json_string(item, "brand", "unknown");
        reading.model = json_string(item, "model", "unknown");
        reading.protocol = json_string(item, "protocol", "unknown");
        reading.parse_status = json_string(item, "parse_status", "cached");
        reading.location = json_string(item, "location");
        reading.debug = json_string(item, "debug");
        reading.rssi = static_cast<int>(json_number(item, "rssi", 0));
        reading.last_seen = static_cast<uint64_t>(json_number(item, "last_seen", 0));
        reading.raw_advertisement_hex = json_string(item, "raw_advertisement");
        reading.has_temperature = json_bool(item, "has_temperature");
        reading.has_humidity = json_bool(item, "has_humidity");
        reading.has_battery = json_bool(item, "has_battery");
        reading.has_external_probe_temperature = json_bool(item, "has_external_probe_temperature");
        reading.temperature_c = static_cast<float>(json_number(item, "temperature_c", 0));
        reading.humidity_percent = static_cast<float>(json_number(item, "humidity_percent", 0));
        reading.battery_percent = static_cast<int>(json_number(item, "battery_percent", 0));
        reading.external_probe_temperature_c = static_cast<float>(json_number(item, "external_probe_temperature_c", 0));
        readings.push_back(reading);
    }
    cJSON_Delete(root);
    return readings;
}

esp_err_t NvsStore::save_ble_sensor_last_readings(const std::vector<BleSensorLastReading>& readings) {
    if (readings.size() > 32) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON* root = cJSON_CreateArray();
    for (const auto& reading : readings) {
        if (reading.mac_address.empty()) {
            continue;
        }
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", reading.mac_address.c_str());
        cJSON_AddStringToObject(item, "name", reading.name.c_str());
        cJSON_AddStringToObject(item, "display_name", reading.display_name.c_str());
        cJSON_AddStringToObject(item, "brand", reading.brand.c_str());
        cJSON_AddStringToObject(item, "model", reading.model.c_str());
        cJSON_AddStringToObject(item, "protocol", reading.protocol.c_str());
        cJSON_AddStringToObject(item, "parse_status", reading.parse_status.c_str());
        cJSON_AddStringToObject(item, "location", reading.location.c_str());
        cJSON_AddStringToObject(item, "debug", reading.debug.c_str());
        cJSON_AddNumberToObject(item, "rssi", reading.rssi);
        cJSON_AddNumberToObject(item, "last_seen", static_cast<double>(reading.last_seen));
        cJSON_AddStringToObject(item, "raw_advertisement", reading.raw_advertisement_hex.c_str());
        cJSON_AddBoolToObject(item, "has_temperature", reading.has_temperature);
        cJSON_AddBoolToObject(item, "has_humidity", reading.has_humidity);
        cJSON_AddBoolToObject(item, "has_battery", reading.has_battery);
        cJSON_AddBoolToObject(item, "has_external_probe_temperature", reading.has_external_probe_temperature);
        if (reading.has_temperature) cJSON_AddNumberToObject(item, "temperature_c", reading.temperature_c);
        if (reading.has_humidity) cJSON_AddNumberToObject(item, "humidity_percent", reading.humidity_percent);
        if (reading.has_battery) cJSON_AddNumberToObject(item, "battery_percent", reading.battery_percent);
        if (reading.has_external_probe_temperature) {
            cJSON_AddNumberToObject(item, "external_probe_temperature_c", reading.external_probe_temperature_c);
        }
        cJSON_AddItemToArray(root, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string value = text ? text : "[]";
    cJSON_free(text);
    cJSON_Delete(root);
    return set_string(KEY_BLE_SENSOR_LAST, value);
}

std::vector<PairedUniversalDeviceConfig> NvsStore::get_universal_devices() {
    std::vector<PairedUniversalDeviceConfig> devices;
    std::string json;
    if (get_string(KEY_UNIVERSAL_DEVICES, json) != ESP_OK || json.empty()) return devices;
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return devices;
    }
    const int count = cJSON_GetArraySize(root);
    devices.reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        cJSON* id = cJSON_GetObjectItem(item, "id");
        cJSON* address = cJSON_GetObjectItem(item, "address");
        if (!cJSON_IsString(id) || !cJSON_IsString(address)) continue;
        PairedUniversalDeviceConfig device;
        device.id = id->valuestring;
        device.address = address->valuestring;
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* type = cJSON_GetObjectItem(item, "type");
        cJSON* brand = cJSON_GetObjectItem(item, "brand");
        cJSON* model = cJSON_GetObjectItem(item, "model");
        cJSON* protocol = cJSON_GetObjectItem(item, "protocol");
        cJSON* location = cJSON_GetObjectItem(item, "location");
        device.name = cJSON_IsString(name) ? name->valuestring : device.id;
        device.type = cJSON_IsString(type) ? type->valuestring : "unknown";
        device.brand = cJSON_IsString(brand) ? brand->valuestring : "generic";
        device.model = cJSON_IsString(model) ? model->valuestring : "unknown";
        device.protocol = cJSON_IsString(protocol) ? protocol->valuestring : "unknown";
        device.location = cJSON_IsString(location) ? location->valuestring : "";
        devices.push_back(device);
    }
    cJSON_Delete(root);
    return devices;
}

esp_err_t NvsStore::save_universal_devices(const std::vector<PairedUniversalDeviceConfig>& devices) {
    if (devices.size() > 32) return ESP_ERR_INVALID_ARG;
    cJSON* root = cJSON_CreateArray();
    for (const auto& device : devices) {
        if (device.id.empty() || device.address.empty()) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", device.id.c_str());
        cJSON_AddStringToObject(item, "name", device.name.c_str());
        cJSON_AddStringToObject(item, "type", device.type.c_str());
        cJSON_AddStringToObject(item, "brand", device.brand.c_str());
        cJSON_AddStringToObject(item, "model", device.model.c_str());
        cJSON_AddStringToObject(item, "protocol", device.protocol.c_str());
        cJSON_AddStringToObject(item, "address", device.address.c_str());
        cJSON_AddStringToObject(item, "location", device.location.c_str());
        cJSON_AddItemToArray(root, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string value = text ? text : "[]";
    cJSON_free(text);
    cJSON_Delete(root);
    return set_string(KEY_UNIVERSAL_DEVICES, value);
}

esp_err_t NvsStore::get_string(const char* key, std::string& value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required = 0;
    err = nvs_get_str(handle, key, nullptr, &required);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    value.assign(required, '\0');
    err = nvs_get_str(handle, key, value.data(), &required);
    nvs_close(handle);
    if (err == ESP_OK && !value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return err;
}

esp_err_t NvsStore::set_string(const char* key, const std::string& value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, key, value.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t NvsStore::get_i32(const char* key, int32_t& value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_i32(handle, key, &value);
    nvs_close(handle);
    return err;
}

esp_err_t NvsStore::set_i32(const char* key, int32_t value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

NvsStore& nvs_store() {
    static NvsStore store;
    return store;
}

} // namespace tigeros
