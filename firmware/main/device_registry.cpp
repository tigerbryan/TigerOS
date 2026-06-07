#include "device_registry.h"

#include <algorithm>
#include <cctype>

#include "ble_sensor_gateway.h"
#include "cJSON.h"
#include "device_manager.h"
#include "esp_timer.h"
#include "mqtt_manager.h"
#include "nvs_store.h"
#include "tiger_log.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "device_registry";
constexpr size_t MAX_DISCOVERED_DEVICES_API = 24;
constexpr uint64_t BLE_ONLINE_WINDOW_SECONDS = 180;

std::string sanitize_id_part(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            out.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
        } else if (ch == '-' || ch == '_') {
            out.push_back(ch);
        }
    }
    return out.empty() ? "unknown" : out;
}

std::string normalized_address_key(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isxdigit(ch)) {
            out.push_back(static_cast<char>(std::toupper(ch)));
        }
    }
    return out;
}

uint64_t uptime_seconds() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
}

bool ble_reading_is_fresh(uint64_t last_seen) {
    const uint64_t now = uptime_seconds();
    return last_seen > 0 && now >= last_seen && now - last_seen < BLE_ONLINE_WINDOW_SECONDS;
}

std::string json_print(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    return out;
}

UniversalDevice from_ble_reading(const BleSensorReading& reading, bool discovered) {
    UniversalDevice device;
    device.id = universal_device_id_from_address("ble", reading.mac_address);
    device.name = reading.display_name.empty() ? (reading.name.empty() ? reading.mac_address : reading.name) : reading.display_name;
    device.type = "sensor";
    device.brand = reading.brand.empty() ? "generic" : reading.brand;
    device.model = reading.model.empty() ? "unknown" : reading.model;
    device.protocol = "ble";
    device.adapter = "ble_sensor";
    device.address = reading.mac_address;
    device.location = reading.location;
    device.online = ble_reading_is_fresh(reading.last_seen) && reading.parse_status != "offline" && reading.parse_status != "cached";
    device.last_seen = reading.last_seen;
    if (reading.has_temperature) device.capabilities.push_back("temperature");
    if (reading.has_humidity) device.capabilities.push_back("humidity");
    if (reading.has_battery) device.capabilities.push_back("battery");
    if (reading.has_external_probe_temperature) device.capabilities.push_back("external_probe_temperature");
    if (device.capabilities.empty() && discovered) device.capabilities.push_back("raw");

    cJSON* state = cJSON_CreateObject();
    if (reading.has_temperature) cJSON_AddNumberToObject(state, "temperature_c", reading.temperature_c);
    if (reading.has_humidity) cJSON_AddNumberToObject(state, "humidity_percent", reading.humidity_percent);
    if (reading.has_battery) cJSON_AddNumberToObject(state, "battery_percent", reading.battery_percent);
    if (reading.has_external_probe_temperature) cJSON_AddNumberToObject(state, "external_probe_temperature_c", reading.external_probe_temperature_c);
    device.state_json = json_print(state);
    cJSON_Delete(state);

    cJSON* raw = cJSON_CreateObject();
    cJSON_AddStringToObject(raw, "ble_name", reading.name.c_str());
    cJSON_AddStringToObject(raw, "parser_protocol", reading.protocol.c_str());
    cJSON_AddStringToObject(raw, "parse_status", reading.parse_status.c_str());
    cJSON_AddStringToObject(raw, "raw_adv_hex", reading.raw_advertisement_hex.c_str());
    cJSON_AddStringToObject(raw, "debug", reading.debug.c_str());
    cJSON_AddNumberToObject(raw, "rssi", reading.rssi);
    cJSON_AddNumberToObject(raw, "last_seen", static_cast<double>(reading.last_seen));
    device.raw_json = json_print(raw);
    cJSON_Delete(raw);
    return device;
}

UniversalDevice from_universal_config(const PairedUniversalDeviceConfig& config) {
    UniversalDevice device;
    device.id = config.id;
    device.name = config.name.empty() ? config.id : config.name;
    device.type = config.type.empty() ? "unknown" : config.type;
    device.brand = config.brand.empty() ? "generic" : config.brand;
    device.model = config.model.empty() ? "unknown" : config.model;
    device.protocol = config.protocol.empty() ? "unknown" : config.protocol;
    device.adapter = (device.protocol == "ble" || device.protocol == "ble_raw" || device.protocol == "inkbird" ||
                      device.protocol == "bthome" || device.protocol == "pvvx" || device.protocol == "atc") ? "ble_sensor" :
                     (device.protocol == "http" ? "http_device" : "generic");
    device.address = config.address;
    device.location = config.location;
    device.online = false;
    cJSON* raw = cJSON_CreateObject();
    cJSON_AddStringToObject(raw, "address", config.address.c_str());
    cJSON_AddStringToObject(raw, "note", "Manual universal device placeholder; protocol polling adapter not enabled");
    device.raw_json = json_print(raw);
    cJSON_Delete(raw);
    return device;
}

UniversalDevice from_universal_config_with_cached_reading(const PairedUniversalDeviceConfig& config, const BleSensorLastReading& reading) {
    UniversalDevice device;
    device.id = config.id;
    device.name = config.name.empty() ? (reading.display_name.empty() ? config.id : reading.display_name) : config.name;
    device.type = config.type.empty() ? "sensor" : config.type;
    device.brand = (reading.brand.empty() || reading.brand == "unknown") ? config.brand : reading.brand;
    device.model = (reading.model.empty() || reading.model == "unknown") ? config.model : reading.model;
    device.protocol = (reading.protocol.empty() || reading.protocol == "unknown") ? config.protocol : reading.protocol;
    device.adapter = "ble_sensor";
    device.address = config.address;
    device.location = config.location.empty() ? reading.location : config.location;
    device.online = false;
    device.last_seen = 0;
    if (reading.has_temperature) device.capabilities.push_back("temperature");
    if (reading.has_humidity) device.capabilities.push_back("humidity");
    if (reading.has_battery) device.capabilities.push_back("battery");
    if (reading.has_external_probe_temperature) device.capabilities.push_back("external_probe_temperature");

    cJSON* state = cJSON_CreateObject();
    if (reading.has_temperature) cJSON_AddNumberToObject(state, "temperature_c", reading.temperature_c);
    if (reading.has_humidity) cJSON_AddNumberToObject(state, "humidity_percent", reading.humidity_percent);
    if (reading.has_battery) cJSON_AddNumberToObject(state, "battery_percent", reading.battery_percent);
    if (reading.has_external_probe_temperature) {
        cJSON_AddNumberToObject(state, "external_probe_temperature_c", reading.external_probe_temperature_c);
    }
    device.state_json = json_print(state);
    cJSON_Delete(state);

    cJSON* raw = cJSON_CreateObject();
    cJSON_AddStringToObject(raw, "address", config.address.c_str());
    cJSON_AddStringToObject(raw, "parser_protocol", device.protocol.c_str());
    cJSON_AddStringToObject(raw, "parse_status", "cached");
    cJSON_AddStringToObject(raw, "raw_adv_hex", reading.raw_advertisement_hex.c_str());
    cJSON_AddStringToObject(raw, "note", "Last cached BLE reading; waiting for next advertisement");
    cJSON_AddNumberToObject(raw, "rssi", reading.rssi);
    cJSON_AddNumberToObject(raw, "cached_last_seen", static_cast<double>(reading.last_seen));
    device.raw_json = json_print(raw);
    cJSON_Delete(raw);
    return device;
}

UniversalDevice from_universal_config_with_ble_reading(const PairedUniversalDeviceConfig& config, const BleSensorReading& reading) {
    UniversalDevice device = from_ble_reading(reading, true);
    device.id = config.id;
    device.name = config.name.empty() ? device.name : config.name;
    device.type = config.type.empty() ? "sensor" : config.type;
    device.protocol = (!reading.protocol.empty() && reading.protocol != "unknown") ? reading.protocol :
                      (config.protocol.empty() ? "ble_raw" : config.protocol);
    device.adapter = "ble_sensor";
    device.address = config.address.empty() ? reading.mac_address : config.address;
    device.location = config.location;
    if (!reading.brand.empty() && reading.brand != "generic" && reading.brand != "unknown") device.brand = reading.brand;
    else if (!config.brand.empty() && config.brand != "generic" && config.brand != "unknown") device.brand = config.brand;
    if (!reading.model.empty() && reading.model != "manual" && reading.model != "unknown") device.model = reading.model;
    else if (!config.model.empty() && config.model != "manual" && config.model != "unknown") device.model = config.model;
    if (device.capabilities.empty()) device.capabilities.push_back("raw");
    return device;
}

bool reading_has_values(const BleSensorReading& reading) {
    return reading.has_temperature || reading.has_humidity || reading.has_battery || reading.has_external_probe_temperature;
}

bool reading_is_actionable_discovery(const BleSensorReading& reading) {
    if (reading.paired || reading_has_values(reading)) {
        return true;
    }
    if (reading.parse_status == "encrypted" || reading.parse_status == "partial") {
        return true;
    }
    if (reading.protocol == "bthome" || reading.protocol == "pvvx" || reading.protocol == "atc" ||
        reading.protocol == "xiaomi" || reading.protocol == "inkbird") {
        return true;
    }
    if (reading.name.rfind("ATC", 0) == 0 || reading.name.rfind("PVVX", 0) == 0 || reading.name.find("LYWSD") != std::string::npos) {
        return true;
    }
    return false;
}

bool config_is_ble_backed(const PairedUniversalDeviceConfig& config) {
    return config.protocol == "ble_raw" || config.protocol == "ble" || config.protocol == "inkbird" ||
           config.protocol == "bthome" || config.protocol == "pvvx" || config.protocol == "atc";
}

std::optional<std::string> address_from_universal_id(const std::string& id) {
    constexpr const char* prefix = "ble-";
    if (id.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    std::string hex = id.substr(4);
    if (hex.size() != 12) {
        return std::nullopt;
    }
    std::string mac;
    for (size_t i = 0; i < hex.size(); i += 2) {
        if (!mac.empty()) mac.push_back(':');
        mac += hex.substr(i, 2);
    }
    std::transform(mac.begin(), mac.end(), mac.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return mac;
}

} // namespace

esp_err_t DeviceRegistry::init() {
    tiger_log("INFO", TAG, "Universal device registry initialized");
    return ESP_OK;
}

std::vector<UniversalDevice> DeviceRegistry::devices() {
    std::vector<UniversalDevice> out;
    out.reserve(8);
    for (const auto& reading : ble_sensor_gateway().paired_latest()) {
        out.push_back(from_ble_reading(reading, false));
    }
    const auto discovered_ble = ble_sensor_gateway().discovered();
    const auto cached_ble = nvs_store().get_ble_sensor_last_readings();
    for (const auto& config : nvs_store().get_universal_devices()) {
        if (config_is_ble_backed(config)) {
            const std::string wanted = normalized_address_key(config.address);
            auto it = std::find_if(discovered_ble.begin(), discovered_ble.end(), [&](const auto& reading) {
                return !wanted.empty() && normalized_address_key(reading.mac_address) == wanted;
            });
            if (it != discovered_ble.end()) {
                out.push_back(from_universal_config_with_ble_reading(config, *it));
                continue;
            }
            auto cached = std::find_if(cached_ble.begin(), cached_ble.end(), [&](const auto& reading) {
                return !wanted.empty() && normalized_address_key(reading.mac_address) == wanted;
            });
            if (cached != cached_ble.end()) {
                out.push_back(from_universal_config_with_cached_reading(config, *cached));
                continue;
            }
        }
        out.push_back(from_universal_config(config));
    }
    return out;
}

std::vector<UniversalDevice> DeviceRegistry::discovered() {
    std::vector<UniversalDevice> out;
    out.reserve(8);
    for (const auto& reading : ble_sensor_gateway().discovered()) {
        if (!reading_is_actionable_discovery(reading)) {
            continue;
        }
        out.push_back(from_ble_reading(reading, true));
        if (out.size() >= MAX_DISCOVERED_DEVICES_API) {
            break;
        }
    }
    return out;
}

std::optional<UniversalDevice> DeviceRegistry::get(const std::string& id) {
    for (const auto& device : devices()) {
        if (device.id == id) return device;
    }
    for (const auto& device : discovered()) {
        if (device.id == id) return device;
    }
    return std::nullopt;
}

esp_err_t DeviceRegistry::scan(const std::string& method) {
    if (method.empty() || method == "ble") {
        return ble_sensor_gateway().request_scan();
    }
    // MQTT and HTTP discovery adapters are intentionally lightweight
    // placeholders in V1.0; they do not allocate protocol clients until enabled.
    tiger_log("INFO", TAG, "Discovery method placeholder accepted");
    return ESP_OK;
}

esp_err_t DeviceRegistry::pair(cJSON* payload) {
    cJSON* protocol = cJSON_GetObjectItem(payload, "protocol");
    cJSON* id = cJSON_GetObjectItem(payload, "id");
    cJSON* address = cJSON_GetObjectItem(payload, "address");
    cJSON* mac = cJSON_GetObjectItem(payload, "mac");
    const std::string proto = cJSON_IsString(protocol) ? protocol->valuestring : "ble";
    if (proto != "ble") {
        cJSON* address = cJSON_GetObjectItem(payload, "address");
        if (!cJSON_IsString(address) || address->valuestring[0] == '\0') return ESP_ERR_INVALID_ARG;
        PairedUniversalDeviceConfig config;
        config.protocol = proto;
        config.address = address->valuestring;
        config.id = universal_device_id_from_address(proto, config.address);
        cJSON* name = cJSON_GetObjectItem(payload, "name");
        cJSON* type = cJSON_GetObjectItem(payload, "type");
        cJSON* brand = cJSON_GetObjectItem(payload, "brand");
        cJSON* model = cJSON_GetObjectItem(payload, "model");
        cJSON* location = cJSON_GetObjectItem(payload, "location");
        config.name = cJSON_IsString(name) ? name->valuestring : config.id;
        config.type = cJSON_IsString(type) ? type->valuestring : "unknown";
        config.brand = cJSON_IsString(brand) ? brand->valuestring : "generic";
        config.model = cJSON_IsString(model) ? model->valuestring : "manual";
        config.location = cJSON_IsString(location) ? location->valuestring : "";
        auto devices = nvs_store().get_universal_devices();
        auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& item) { return item.id == config.id; });
        if (it == devices.end()) devices.push_back(config); else *it = config;
        return nvs_store().save_universal_devices(devices);
    }
    std::string ble_address;
    if (cJSON_IsString(address) && address->valuestring[0] != '\0') {
        ble_address = address->valuestring;
    } else if (cJSON_IsString(mac) && mac->valuestring[0] != '\0') {
        ble_address = mac->valuestring;
    } else if (cJSON_IsString(id)) {
        auto parsed = address_from_universal_id(id->valuestring);
        if (parsed) ble_address = *parsed;
    }
    if (ble_address.empty()) {
        return ESP_ERR_INVALID_ARG;
    }

    PairedBleSensorConfig config;
    config.mac_address = ble_address;
    cJSON* name = cJSON_GetObjectItem(payload, "name");
    cJSON* brand = cJSON_GetObjectItem(payload, "brand");
    cJSON* model = cJSON_GetObjectItem(payload, "model");
    cJSON* location = cJSON_GetObjectItem(payload, "location");
    cJSON* parser_protocol = cJSON_GetObjectItem(payload, "parser_protocol");
    config.name = cJSON_IsString(name) ? name->valuestring : ble_address;
    config.brand = cJSON_IsString(brand) ? brand->valuestring : "unknown";
    config.model = cJSON_IsString(model) ? model->valuestring : "unknown";
    config.protocol = cJSON_IsString(parser_protocol) ? parser_protocol->valuestring : "ble";
    config.location = cJSON_IsString(location) ? location->valuestring : "";
    return ble_sensor_gateway().pair_sensor(config);
}

esp_err_t DeviceRegistry::remove(const std::string& id) {
    auto address = address_from_universal_id(id);
    if (address) return ble_sensor_gateway().remove_sensor(*address);
    auto devices = nvs_store().get_universal_devices();
    const auto old_size = devices.size();
    devices.erase(std::remove_if(devices.begin(), devices.end(), [&](const auto& item) { return item.id == id; }), devices.end());
    if (devices.size() == old_size) return ESP_ERR_NOT_FOUND;
    return nvs_store().save_universal_devices(devices);
}

esp_err_t DeviceRegistry::rename(const std::string& id, const std::string& name) {
    auto address = address_from_universal_id(id);
    if (address) return ble_sensor_gateway().rename_sensor(*address, name);
    auto devices = nvs_store().get_universal_devices();
    for (auto& item : devices) {
        if (item.id == id) {
            item.name = name;
            return nvs_store().save_universal_devices(devices);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t DeviceRegistry::set_location(const std::string& id, const std::string& location) {
    auto address = address_from_universal_id(id);
    if (address) return ble_sensor_gateway().set_location(*address, location);
    auto devices = nvs_store().get_universal_devices();
    for (auto& item : devices) {
        if (item.id == id) {
            item.location = location;
            return nvs_store().save_universal_devices(devices);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t DeviceRegistry::control(const std::string& id, const std::string&, cJSON*) {
    auto device = get(id);
    if (!device) return ESP_ERR_NOT_FOUND;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t DeviceRegistry::publish_state(const UniversalDevice& device) {
    auto status = device_manager().status();
    cJSON* root = universal_device_to_json(device, false);
    cJSON_AddStringToObject(root, "schema", "tigeros.device_state.v1");
    cJSON_AddStringToObject(root, "gateway_id", status.device_id.c_str());
    cJSON_AddStringToObject(root, "source", "tigeros_gateway");
    cJSON_AddNumberToObject(root, "collected_at_uptime", static_cast<double>(device.last_seen));
    cJSON_AddNumberToObject(root, "sent_at_uptime", static_cast<double>(status.uptime_seconds));
    cJSON_AddNumberToObject(root, "gateway_uptime", static_cast<double>(status.uptime_seconds));
    char* text = cJSON_PrintUnformatted(root);
    std::string topic = "tigeros/" + status.device_id + "/devices/" + device.id + "/state";
    esp_err_t err = mqtt_manager().publish_raw(topic, text ? text : "{}", 0, false);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

std::string universal_device_id_from_address(const std::string& protocol, const std::string& address) {
    return sanitize_id_part(protocol) + "-" + sanitize_id_part(address);
}

cJSON* universal_device_to_json(const UniversalDevice& device, bool include_raw) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", device.id.c_str());
    cJSON_AddStringToObject(root, "device_id", device.id.c_str());
    cJSON_AddStringToObject(root, "name", device.name.c_str());
    cJSON_AddStringToObject(root, "type", device.type.c_str());
    cJSON_AddStringToObject(root, "brand", device.brand.c_str());
    cJSON_AddStringToObject(root, "model", device.model.c_str());
    cJSON_AddStringToObject(root, "protocol", device.protocol.c_str());
    cJSON_AddStringToObject(root, "adapter", device.adapter.c_str());
    cJSON_AddStringToObject(root, "address", device.address.c_str());
    cJSON_AddStringToObject(root, "location", device.location.c_str());
    cJSON_AddBoolToObject(root, "online", device.online);
    cJSON_AddNumberToObject(root, "last_seen", static_cast<double>(device.last_seen));
    cJSON* caps = cJSON_AddArrayToObject(root, "capabilities");
    for (const auto& cap : device.capabilities) {
        cJSON_AddItemToArray(caps, cJSON_CreateString(cap.c_str()));
    }
    cJSON* state = cJSON_Parse(device.state_json.c_str());
    cJSON_AddItemToObject(root, "state", state ? state : cJSON_CreateObject());
    if (include_raw) {
        cJSON* raw = cJSON_Parse(device.raw_json.c_str());
        cJSON_AddItemToObject(root, "raw", raw ? raw : cJSON_CreateObject());
    }
    return root;
}

DeviceRegistry& device_registry() {
    static DeviceRegistry registry;
    return registry;
}

} // namespace tigeros
