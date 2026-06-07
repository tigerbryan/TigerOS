#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "esp_err.h"

namespace tigeros {

struct WifiCredentials {
    std::string ssid;
    std::string password;
};

struct MqttConfig {
    bool enabled = false;
    std::string host;
    int port = 1883;
    std::string username;
    std::string password;
    std::string client_id;
    bool use_tls = false;
    bool ha_discovery_enabled = true;
    std::string ha_discovery_prefix = "homeassistant";
};

struct BleConfig {
    bool enabled = true;
    std::string pairing_pin = "123456";
    std::string pop_token;
};

struct PairedBleSensorConfig {
    std::string mac_address;
    std::string name;
    std::string brand = "unknown";
    std::string model = "unknown";
    std::string protocol = "unknown";
    std::string location;
    std::string bindkey;
};

struct BleSensorLastReading {
    std::string mac_address;
    std::string name;
    std::string display_name;
    std::string brand = "unknown";
    std::string model = "unknown";
    std::string protocol = "unknown";
    std::string parse_status = "cached";
    std::string location;
    std::string debug;
    int rssi = 0;
    uint64_t last_seen = 0;
    std::string raw_advertisement_hex;
    bool has_temperature = false;
    bool has_humidity = false;
    bool has_battery = false;
    bool has_external_probe_temperature = false;
    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
    int battery_percent = 0;
    float external_probe_temperature_c = 0.0f;
};

struct PairedUniversalDeviceConfig {
    std::string id;
    std::string name;
    std::string type = "unknown";
    std::string brand = "generic";
    std::string model = "unknown";
    std::string protocol = "unknown";
    std::string address;
    std::string location;
};

struct OtaConfig {
    bool enabled = false;
    bool auto_update = false;
    std::string check_url;
    std::string channel = "stable";
};

struct WebhookConfig {
    bool enabled = false;
    std::string url;
    std::string secret_header = "x-ingest-secret";
    std::string secret_value;
    int interval_seconds = 60;
};

class NvsStore {
public:
    esp_err_t init();

    std::optional<WifiCredentials> get_wifi_credentials();
    esp_err_t save_wifi_credentials(const std::string& ssid, const std::string& password);
    esp_err_t clear_wifi_credentials();
    esp_err_t factory_reset_config();

    std::string get_or_create_device_id();
    std::string get_firmware_version();
    esp_err_t ensure_firmware_version(const std::string& version);

    std::string get_cloud_token();
    esp_err_t save_cloud_token(const std::string& token);
    esp_err_t clear_cloud_token();

    std::string get_or_create_api_token();
    std::string get_password_hash();
    esp_err_t save_password_hash(const std::string& hash);

    std::string get_ota_check_url();
    esp_err_t save_ota_check_url(const std::string& url);
    std::string get_ota_channel();
    esp_err_t save_ota_channel(const std::string& channel);
    OtaConfig get_ota_config();
    esp_err_t save_ota_config(const OtaConfig& config);

    MqttConfig get_mqtt_config();
    esp_err_t save_mqtt_config(const MqttConfig& config);

    BleConfig get_ble_config();
    esp_err_t save_ble_config(const BleConfig& config);

    WebhookConfig get_webhook_config();
    esp_err_t save_webhook_config(const WebhookConfig& config);

    std::vector<PairedBleSensorConfig> get_ble_sensor_pairs();
    esp_err_t save_ble_sensor_pairs(const std::vector<PairedBleSensorConfig>& sensors);
    std::vector<BleSensorLastReading> get_ble_sensor_last_readings();
    esp_err_t save_ble_sensor_last_readings(const std::vector<BleSensorLastReading>& readings);
    std::vector<PairedUniversalDeviceConfig> get_universal_devices();
    esp_err_t save_universal_devices(const std::vector<PairedUniversalDeviceConfig>& devices);

private:
    esp_err_t get_string(const char* key, std::string& value);
    esp_err_t set_string(const char* key, const std::string& value);
    esp_err_t get_i32(const char* key, int32_t& value);
    esp_err_t set_i32(const char* key, int32_t value);
};

NvsStore& nvs_store();

} // namespace tigeros
