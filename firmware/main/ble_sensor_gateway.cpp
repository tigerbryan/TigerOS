#include "ble_sensor_gateway.h"

#include <algorithm>
#include <cstdio>

#include "ble_manager.h"
#include "ble_sensor_registry.h"
#include "cJSON.h"
#include "device_manager.h"
#include "device_registry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "mqtt_manager.h"
#include "os/os_mbuf.h"
#include "tiger_log.h"
#include "wifi_manager.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "ble_sensor_gateway";
constexpr uint32_t MANUAL_SCAN_WINDOW_MS = 45000;
constexpr uint32_t AUTO_SCAN_WINDOW_MS = 20000;
constexpr uint64_t GATT_INSPECT_TIMEOUT_SECONDS = 25;
constexpr uint64_t SCAN_INTERVAL_SECONDS = 60;
constexpr uint64_t MQTT_PUBLISH_SECONDS = 60;
constexpr size_t MAX_DISCOVERED = 64;
constexpr size_t MAX_RAW_PACKETS = 64;
constexpr size_t MAX_UNKNOWN_RAW_SAMPLES = 12;

bool has_watched_ble_devices() {
    if (ble_sensor_gateway().paired_count() > 0) {
        return true;
    }
    for (const auto& device : nvs_store().get_universal_devices()) {
        if (device.protocol == "ble" || device.protocol == "ble_raw" || device.protocol == "inkbird" ||
            device.protocol == "bthome" || device.protocol == "pvvx" || device.protocol == "atc") {
            return true;
        }
    }
    return false;
}

bool is_watched_ble_address(const std::string& mac) {
    for (const auto& cfg : nvs_store().get_ble_sensor_pairs()) {
        if (cfg.mac_address == mac) {
            return true;
        }
    }
    for (const auto& device : nvs_store().get_universal_devices()) {
        if ((device.protocol == "ble" || device.protocol == "ble_raw" || device.protocol == "inkbird" ||
             device.protocol == "bthome" || device.protocol == "pvvx" || device.protocol == "atc") &&
            device.address == mac) {
            return true;
        }
    }
    return false;
}

class ScopedLock {
public:
    explicit ScopedLock(SemaphoreHandle_t lock) : lock_(lock) {
        if (lock_) {
            xSemaphoreTake(lock_, portMAX_DELAY);
        }
    }
    ~ScopedLock() {
        if (lock_) {
            xSemaphoreGive(lock_);
        }
    }

private:
    SemaphoreHandle_t lock_;
};

uint64_t uptime_seconds() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
}

std::string mac_to_string(const std::array<uint8_t, 6>& addr) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return text;
}

std::string address_type_text(uint8_t type) {
    switch (type) {
        case BLE_ADDR_PUBLIC:
            return "public";
        case BLE_ADDR_RANDOM:
            return "random";
        case BLE_ADDR_PUBLIC_ID:
            return "public_id";
        case BLE_ADDR_RANDOM_ID:
            return "random_id";
        default:
            return "unknown";
    }
}

bool address_type_from_text(const std::string& text, uint8_t& type) {
    if (text == "public") {
        type = BLE_ADDR_PUBLIC;
        return true;
    }
    if (text == "random") {
        type = BLE_ADDR_RANDOM;
        return true;
    }
    if (text == "public_id") {
        type = BLE_ADDR_PUBLIC_ID;
        return true;
    }
    if (text == "random_id") {
        type = BLE_ADDR_RANDOM_ID;
        return true;
    }
    return false;
}

bool parse_mac_string(const std::string& mac, std::array<uint8_t, 6>& out) {
    unsigned int bytes[6] = {};
    if (std::sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
                    &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }
    for (int index = 0; index < 6; ++index) {
        if (bytes[index] > 0xff) {
            return false;
        }
    }
    for (int index = 0; index < 6; ++index) {
        out[5 - index] = static_cast<uint8_t>(bytes[index]);
    }
    return true;
}

std::string uuid_to_text(const ble_uuid_t* uuid) {
    char text[BLE_UUID_STR_LEN] = {};
    ble_uuid_to_str(uuid, text);
    return text;
}

std::string mbuf_to_hex(const os_mbuf* om) {
    if (!om) {
        return {};
    }
    const uint16_t len = OS_MBUF_PKTLEN(om);
    if (len == 0) {
        return {};
    }
    std::vector<uint8_t> data(len);
    if (os_mbuf_copydata(om, 0, len, data.data()) != 0) {
        return {};
    }
    return bytes_to_hex(data);
}

std::string uuid16_to_text(uint16_t uuid) {
    char text[8];
    std::snprintf(text, sizeof(text), "0x%04X", uuid);
    return text;
}

void extract_ad_metadata(const std::vector<BleAdField>& fields,
                         std::string& manufacturer_hex,
                         std::string& service_data_hex,
                         std::vector<std::string>& service_uuids) {
    for (const auto& field : fields) {
        if (field.type == 0xFF) {
            manufacturer_hex = bytes_to_hex(field.data);
        } else if (field.type == 0x16) {
            service_data_hex = bytes_to_hex(field.data);
            if (field.data.size() >= 2) {
                service_uuids.push_back(uuid16_to_text(read_le_u16(field.data.data())));
            }
        } else if ((field.type == 0x02 || field.type == 0x03) && field.data.size() >= 2) {
            for (size_t index = 0; index + 1 < field.data.size(); index += 2) {
                service_uuids.push_back(uuid16_to_text(read_le_u16(field.data.data() + index)));
            }
        }
    }
}

bool likely_sensor_name(const std::string& name) {
    return name == "sps" || name == "SPS" ||
           name.rfind("ATC", 0) == 0 || name.rfind("PVVX", 0) == 0 || name.find("LYWSD") != std::string::npos ||
           name.find("MHO") != std::string::npos || name.find("TH") != std::string::npos;
}

bool has_known_sensor_service_data(const std::vector<BleAdField>& fields) {
    for (const auto& field : fields) {
        if (field.type != 0x16 || field.data.size() < 2) {
            continue;
        }
        const uint16_t uuid = read_le_u16(field.data.data());
        if (uuid == 0xFCD2 || uuid == 0x181A || uuid == 0xFE95 || uuid == 0xFFF0) {
            return true;
        }
    }
    return false;
}

bool has_readable_values(const BleSensorReading& reading) {
    return reading.has_temperature || reading.has_humidity || reading.has_battery || reading.has_external_probe_temperature;
}

BleSensorReading from_last_reading(const PairedBleSensorConfig& cfg, const BleSensorLastReading& cached) {
    BleSensorReading reading;
    reading.mac_address = cached.mac_address;
    reading.name = cached.name.empty() ? cfg.mac_address : cached.name;
    reading.display_name = cfg.name.empty() ? (cached.display_name.empty() ? reading.name : cached.display_name) : cfg.name;
    reading.brand = cached.brand;
    reading.model = cached.model;
    reading.protocol = cached.protocol;
    reading.parse_status = "cached";
    reading.location = cfg.location.empty() ? cached.location : cfg.location;
    reading.debug = cached.debug;
    reading.rssi = cached.rssi;
    // Cached readings are historical values from a previous scan or even a
    // previous boot. Keep the values for display, but do not mark them fresh.
    reading.last_seen = 0;
    reading.raw_advertisement_hex = cached.raw_advertisement_hex;
    reading.paired = true;
    reading.has_temperature = cached.has_temperature;
    reading.has_humidity = cached.has_humidity;
    reading.has_battery = cached.has_battery;
    reading.has_external_probe_temperature = cached.has_external_probe_temperature;
    reading.temperature_c = cached.temperature_c;
    reading.humidity_percent = cached.humidity_percent;
    reading.battery_percent = cached.battery_percent;
    reading.external_probe_temperature_c = cached.external_probe_temperature_c;
    if (!cfg.brand.empty() && cfg.brand != "unknown" && cfg.brand != "generic") reading.brand = cfg.brand;
    if (!cfg.model.empty() && cfg.model != "unknown" && cfg.model != "manual") reading.model = cfg.model;
    if (!cfg.protocol.empty() && cfg.protocol != "unknown" && cfg.protocol != "ble_raw" && cfg.protocol != "ble") {
        reading.protocol = cfg.protocol;
    }
    return reading;
}

BleSensorLastReading to_last_reading(const BleSensorReading& reading) {
    BleSensorLastReading cached;
    cached.mac_address = reading.mac_address;
    cached.name = reading.name;
    cached.display_name = reading.display_name;
    cached.brand = reading.brand;
    cached.model = reading.model;
    cached.protocol = reading.protocol;
    cached.parse_status = reading.parse_status;
    cached.location = reading.location;
    cached.debug = reading.debug;
    cached.rssi = reading.rssi;
    cached.last_seen = reading.last_seen;
    cached.raw_advertisement_hex = reading.raw_advertisement_hex;
    cached.has_temperature = reading.has_temperature;
    cached.has_humidity = reading.has_humidity;
    cached.has_battery = reading.has_battery;
    cached.has_external_probe_temperature = reading.has_external_probe_temperature;
    cached.temperature_c = reading.temperature_c;
    cached.humidity_percent = reading.humidity_percent;
    cached.battery_percent = reading.battery_percent;
    cached.external_probe_temperature_c = reading.external_probe_temperature_c;
    return cached;
}

void add_reading_json(cJSON* item, const BleSensorReading& reading, bool include_raw) {
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

} // namespace

esp_err_t BleSensorGateway::init() {
    if (initialized_) {
        return ESP_OK;
    }
    lock_ = xSemaphoreCreateMutex();
    if (!lock_) {
        return ESP_ERR_NO_MEM;
    }
    discovered_.reserve(MAX_DISCOVERED);
    raw_packets_.reserve(MAX_RAW_PACKETS);
    initialized_ = true;
    if (!task_started_) {
        xTaskCreate(task_entry, "ble_sensor_gateway", 6144, this, 5, nullptr);
        task_started_ = true;
    }
    tiger_log("INFO", TAG, "BLE sensor gateway initialized");
    return ESP_OK;
}

esp_err_t BleSensorGateway::request_scan() {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    scan_requested_ = true;
    manual_scan_pending_ = true;
    tiger_log("INFO", TAG, "BLE sensor scan requested");
    return ESP_OK;
}

esp_err_t BleSensorGateway::start_scan() {
    return start_scan_window(MANUAL_SCAN_WINDOW_MS, true);
}

esp_err_t BleSensorGateway::start_scan_window(uint32_t window_ms, bool active_scan) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (scanning_) {
        return ESP_OK;
    }
    if (ble_hs_id_infer_auto(0, &own_addr_type_) != 0) {
        tiger_log("WARN", TAG, "BLE sensor scan skipped; host is not synchronized");
        return ESP_ERR_INVALID_STATE;
    }
    ble_manager().stop_provisioning_for_scan();

    struct ble_gap_disc_params params = {};
    // BTHome/PVVX values are broadcast directly. Inkbird devices often expose
    // their short name ("sps") in scan responses, so manual scans remain active.
    params.passive = active_scan ? 0 : 1;
    params.filter_duplicates = 0;
    params.itvl = 0x0050;
    params.window = 0x0050;
    int rc = ble_gap_disc(own_addr_type_, window_ms, &params, gap_event, this);
    if (rc != 0) {
        tiger_log("WARN", TAG, "BLE sensor scan could not start; retry later");
        return ESP_FAIL;
    }

    set_scanning(true);
    last_scan_started_ = uptime_seconds();
    char message[112];
    std::snprintf(message,
                  sizeof(message),
                  "BLE sensor scan started window_ms=%lu mode=%s",
                  static_cast<unsigned long>(window_ms),
                  active_scan ? "active" : "passive");
    tiger_log("INFO", TAG, message);
    return ESP_OK;
}

esp_err_t BleSensorGateway::stop_scan() {
    scan_requested_ = false;
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        return ESP_FAIL;
    }
    set_scanning(false);
    tiger_log("INFO", TAG, "BLE sensor scan stopped");
    return ESP_OK;
}

esp_err_t BleSensorGateway::inspect_gatt(const std::string& mac) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    std::array<uint8_t, 6> parsed_addr{};
    if (!parse_mac_string(mac, parsed_addr)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (scanning_) {
        stop_scan();
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    if (ble_hs_id_infer_auto(0, &own_addr_type_) != 0) {
        tiger_log("WARN", TAG, "BLE GATT inspect skipped; host is not synchronized");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t peer_addr_type = BLE_ADDR_RANDOM;
    std::string peer_addr_type_text = "random";
    {
        ScopedLock lock(lock_);
        if (gatt_inspection_.running) {
            return ESP_ERR_INVALID_STATE;
        }
        for (const auto& packet : raw_packets_) {
            if (packet.mac_address == mac && address_type_from_text(packet.address_type, peer_addr_type)) {
                peer_addr_type_text = packet.address_type;
                break;
            }
        }
        gatt_inspection_ = {};
        gatt_inspection_.running = true;
        gatt_inspection_.mac_address = mac;
        gatt_inspection_.address_type = peer_addr_type_text;
        gatt_inspection_.started_at = uptime_seconds();
        gatt_conn_handle_ = 0xffff;
        gatt_service_index_ = 0;
        gatt_read_service_index_ = 0;
        gatt_read_characteristic_index_ = 0;
    }

    ble_addr_t peer_addr = {};
    peer_addr.type = peer_addr_type;
    std::copy(parsed_addr.begin(), parsed_addr.end(), peer_addr.val);

    ble_manager().stop_provisioning_for_scan();
    int rc = ble_gap_connect(own_addr_type_, &peer_addr, 15000, nullptr, gap_event, this);
    if (rc != 0) {
        finish_gatt_inspection(false, rc, "connect_start_failed");
        return ESP_FAIL;
    }

    char message[128];
    std::snprintf(message, sizeof(message), "BLE GATT inspect started mac=%s type=%s", mac.c_str(), peer_addr_type_text.c_str());
    tiger_log("INFO", TAG, message);
    return ESP_OK;
}

BleGattInspection BleSensorGateway::gatt_inspection() {
    ScopedLock lock(lock_);
    return gatt_inspection_;
}

void BleSensorGateway::pause_auto_scan(const char* reason) {
    auto_scan_paused_ = true;
    stop_scan();
    char message[128];
    std::snprintf(message, sizeof(message), "BLE auto scan paused: %s", reason ? reason : "unspecified");
    tiger_log("INFO", TAG, message);
}

void BleSensorGateway::resume_auto_scan() {
    auto_scan_paused_ = false;
    last_scan_started_ = 0;
    tiger_log("INFO", TAG, "BLE auto scan resumed");
}

bool BleSensorGateway::scanning() const {
    return scanning_;
}

std::vector<BleSensorReading> BleSensorGateway::discovered() {
    auto pairs = nvs_store().get_ble_sensor_pairs();
    ScopedLock lock(lock_);
    std::vector<BleSensorReading> out = discovered_;
    for (auto& reading : out) {
        auto it = std::find_if(pairs.begin(), pairs.end(), [&](const auto& cfg) {
            return cfg.mac_address == reading.mac_address;
        });
        if (it != pairs.end()) {
            reading.paired = true;
            reading.display_name = it->name.empty() ? reading.mac_address : it->name;
            reading.location = it->location;
            if (!it->brand.empty() && it->brand != "unknown" && it->brand != "generic") reading.brand = it->brand;
            if (!it->model.empty() && it->model != "unknown" && it->model != "manual") reading.model = it->model;
            if (!it->protocol.empty() && it->protocol != "unknown" && it->protocol != "ble_raw" && it->protocol != "ble") {
                reading.protocol = it->protocol;
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.last_seen > b.last_seen;
    });
    return out;
}

std::vector<BleSensorReading> BleSensorGateway::paired_latest() {
    auto pairs = nvs_store().get_ble_sensor_pairs();
    auto cached_readings = nvs_store().get_ble_sensor_last_readings();
    ScopedLock lock(lock_);
    std::vector<BleSensorReading> out;
    out.reserve(pairs.size());
    for (const auto& cfg : pairs) {
        auto it = std::find_if(discovered_.begin(), discovered_.end(), [&](const auto& reading) {
            return reading.mac_address == cfg.mac_address;
        });
        if (it != discovered_.end()) {
            BleSensorReading reading = *it;
            reading.paired = true;
            reading.display_name = cfg.name.empty() ? cfg.mac_address : cfg.name;
            reading.location = cfg.location;
            if (!cfg.brand.empty() && cfg.brand != "unknown" && cfg.brand != "generic") reading.brand = cfg.brand;
            if (!cfg.model.empty() && cfg.model != "unknown" && cfg.model != "manual") reading.model = cfg.model;
            if (!cfg.protocol.empty() && cfg.protocol != "unknown" && cfg.protocol != "ble_raw" && cfg.protocol != "ble") {
                reading.protocol = cfg.protocol;
            }
            out.push_back(reading);
        } else {
            auto cached = std::find_if(cached_readings.begin(), cached_readings.end(), [&](const auto& item) {
                return item.mac_address == cfg.mac_address;
            });
            if (cached != cached_readings.end()) {
                out.push_back(from_last_reading(cfg, *cached));
                continue;
            }
            BleSensorReading reading;
            reading.mac_address = cfg.mac_address;
            reading.display_name = cfg.name.empty() ? cfg.mac_address : cfg.name;
            reading.name = cfg.name;
            reading.brand = cfg.brand;
            reading.model = cfg.model;
            reading.protocol = cfg.protocol;
            reading.location = cfg.location;
            reading.parse_status = "offline";
            reading.paired = true;
            out.push_back(reading);
        }
    }
    return out;
}

std::vector<PairedBleSensorConfig> BleSensorGateway::paired_configs() {
    return nvs_store().get_ble_sensor_pairs();
}

esp_err_t BleSensorGateway::pair_sensor(const std::string& mac, const std::string& name, const std::string& type) {
    PairedBleSensorConfig config;
    config.mac_address = mac;
    config.name = name;
    config.protocol = type.empty() ? "unknown" : type;
    return pair_sensor(config);
}

esp_err_t BleSensorGateway::pair_sensor(const PairedBleSensorConfig& config) {
    const std::string mac = config.mac_address;
    if (mac.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    auto pairs = nvs_store().get_ble_sensor_pairs();
    auto it = std::find_if(pairs.begin(), pairs.end(), [&](const auto& cfg) { return cfg.mac_address == mac; });
    PairedBleSensorConfig next = config;
    if (next.name.empty()) next.name = mac;
    if (next.brand.empty()) next.brand = "unknown";
    if (next.model.empty()) next.model = "unknown";
    if (next.protocol.empty()) next.protocol = "unknown";
    if (it == pairs.end()) {
        pairs.push_back(next);
    } else {
        *it = next;
    }
    esp_err_t err = nvs_store().save_ble_sensor_pairs(pairs);
    if (err == ESP_OK) {
        tiger_log("INFO", TAG, "BLE sensor paired");
        request_scan();
    } else {
        tiger_log("ERROR", TAG, esp_err_to_name(err));
    }
    return err;
}

esp_err_t BleSensorGateway::remove_sensor(const std::string& mac) {
    auto pairs = nvs_store().get_ble_sensor_pairs();
    const auto old_size = pairs.size();
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [&](const auto& cfg) {
        return cfg.mac_address == mac;
    }), pairs.end());
    if (pairs.size() == old_size) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = nvs_store().save_ble_sensor_pairs(pairs);
    if (err == ESP_OK) {
        tiger_log("INFO", TAG, "BLE sensor removed");
    }
    return err;
}

esp_err_t BleSensorGateway::rename_sensor(const std::string& mac, const std::string& name) {
    if (mac.empty() || name.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    auto pairs = nvs_store().get_ble_sensor_pairs();
    auto it = std::find_if(pairs.begin(), pairs.end(), [&](const auto& cfg) { return cfg.mac_address == mac; });
    if (it == pairs.end()) {
        return ESP_ERR_NOT_FOUND;
    }
    it->name = name;
    esp_err_t err = nvs_store().save_ble_sensor_pairs(pairs);
    if (err == ESP_OK) {
        tiger_log("INFO", TAG, "BLE sensor renamed");
    }
    return err;
}

esp_err_t BleSensorGateway::set_location(const std::string& mac, const std::string& location) {
    if (mac.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    auto pairs = nvs_store().get_ble_sensor_pairs();
    auto it = std::find_if(pairs.begin(), pairs.end(), [&](const auto& cfg) { return cfg.mac_address == mac; });
    if (it == pairs.end()) {
        return ESP_ERR_NOT_FOUND;
    }
    it->location = location;
    return nvs_store().save_ble_sensor_pairs(pairs);
}

esp_err_t BleSensorGateway::set_bindkey(const std::string& mac, const std::string& bindkey) {
    if (mac.empty() || (!bindkey.empty() && bindkey.size() != 32)) {
        return ESP_ERR_INVALID_ARG;
    }
    auto pairs = nvs_store().get_ble_sensor_pairs();
    auto it = std::find_if(pairs.begin(), pairs.end(), [&](const auto& cfg) { return cfg.mac_address == mac; });
    if (it == pairs.end()) {
        return ESP_ERR_NOT_FOUND;
    }
    it->bindkey = bindkey;
    return nvs_store().save_ble_sensor_pairs(pairs);
}

std::vector<BleSensorReading> BleSensorGateway::raw_packets() {
    return discovered();
}

std::vector<BleRawPacket> BleSensorGateway::raw_packet_history() {
    ScopedLock lock(lock_);
    std::vector<BleRawPacket> out = raw_packets_;
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.last_seen > b.last_seen;
    });
    return out;
}

BleScannerStats BleSensorGateway::stats() {
    BleScannerStats out;
    auto pairs = nvs_store().get_ble_sensor_pairs();
    ScopedLock lock(lock_);
    out.scanning = scanning_;
    out.scan_requested = scan_requested_;
    out.provisioning_advertising = ble_manager().provisioning_advertising();
    out.raw_packet_count = raw_packets_.size();
    out.discovered_device_count = discovered_.size();
    out.paired_device_count = pairs.size();
    out.last_scan_started = last_scan_started_;
    out.last_packet_seen = last_packet_seen_;
    for (const auto& packet : raw_packets_) {
        if (packet.has_temperature || packet.has_humidity || packet.has_battery || packet.has_external_probe_temperature) {
            out.readable_device_count++;
        }
        if (packet.protocol == "bthome") {
            out.bthome_packet_count++;
        }
        if (packet.parse_status == "unknown" || packet.protocol == "unknown" || packet.protocol == "ble_raw") {
            out.unknown_packet_count++;
        }
    }
    return out;
}

void BleSensorGateway::clear_raw_packets() {
    ScopedLock lock(lock_);
    raw_packets_.clear();
    last_packet_seen_ = 0;
    tiger_log("INFO", TAG, "BLE raw packet history cleared");
}

size_t BleSensorGateway::discovered_count() const {
    ScopedLock lock(lock_);
    return discovered_.size();
}

size_t BleSensorGateway::paired_count() {
    return nvs_store().get_ble_sensor_pairs().size();
}

int BleSensorGateway::gap_event(::ble_gap_event* event, void* arg) {
    auto* self = static_cast<BleSensorGateway*>(arg);
    if (!self) {
        return 0;
    }
    self->handle_gap_event(event);
    return 0;
}

void BleSensorGateway::task_entry(void* arg) {
    static_cast<BleSensorGateway*>(arg)->loop();
}

void BleSensorGateway::loop() {
    while (true) {
        const uint64_t now = uptime_seconds();
        if (wifi_manager().status().connected) {
            ble_manager().stop_provisioning_for_scan();
        }
        process_pending_advertisements();
        bool gatt_timed_out = false;
        {
            ScopedLock lock(lock_);
            gatt_timed_out = gatt_inspection_.running && gatt_inspection_.started_at > 0 &&
                             now - gatt_inspection_.started_at > GATT_INSPECT_TIMEOUT_SECONDS;
        }
        if (gatt_timed_out) {
            finish_gatt_inspection(false, ESP_ERR_TIMEOUT, "timeout");
        }
        if (scan_requested_ && !scanning_) {
            const bool manual = manual_scan_pending_;
            if (start_scan_window(manual ? MANUAL_SCAN_WINDOW_MS : AUTO_SCAN_WINDOW_MS, true) == ESP_OK) {
                scan_requested_ = false;
                manual_scan_pending_ = false;
            }
        }
        if (!auto_scan_paused_ && has_watched_ble_devices() && wifi_manager().status().connected && !scanning_ &&
            (last_scan_started_ == 0 || now - last_scan_started_ >= SCAN_INTERVAL_SECONDS)) {
            start_scan_window(AUTO_SCAN_WINDOW_MS, true);
        }
        publish_due();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void BleSensorGateway::handle_gap_event(::ble_gap_event* event) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (gatt_inspection_.running) {
                if (event->connect.status == 0) {
                    {
                        ScopedLock lock(lock_);
                        gatt_inspection_.connected = true;
                        gatt_conn_handle_ = event->connect.conn_handle;
                    }
                    tiger_log("INFO", TAG, "BLE GATT inspect connected; discovering services");
                    int rc = ble_gattc_disc_all_svcs(event->connect.conn_handle, gatt_service_event, this);
                    if (rc != 0) {
                        finish_gatt_inspection(false, rc, "service_discovery_start_failed");
                    }
                } else {
                    finish_gatt_inspection(false, event->connect.status, "connect_failed");
                }
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            if (gatt_inspection_.running || gatt_inspection_.connected) {
                if (gatt_inspection_.running) {
                    finish_gatt_inspection(false, event->disconnect.reason, "disconnected");
                } else {
                    ScopedLock lock(lock_);
                    gatt_inspection_.connected = false;
                    gatt_conn_handle_ = 0xffff;
                }
            }
            break;
        case BLE_GAP_EVENT_DISC:
            queue_advertisement(event->disc);
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            set_scanning(false);
            {
                char message[96];
                std::snprintf(message, sizeof(message), "BLE sensor scan complete discovered=%u", static_cast<unsigned>(discovered_count()));
                tiger_log("INFO", TAG, message);
            }
            break;
        default:
            break;
    }
}

int BleSensorGateway::gatt_service_event(uint16_t conn_handle,
                                         const struct ble_gatt_error* error,
                                         const struct ble_gatt_svc* service,
                                         void* arg) {
    auto* self = static_cast<BleSensorGateway*>(arg);
    if (!self || !error) {
        return 0;
    }
    if (error->status == 0 && service) {
        BleGattServiceInfo info;
        info.start_handle = service->start_handle;
        info.end_handle = service->end_handle;
        info.uuid = uuid_to_text(&service->uuid.u);
        ScopedLock lock(self->lock_);
        self->gatt_inspection_.services.push_back(info);
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        self->start_next_gatt_characteristic_discovery();
        return 0;
    }
    self->finish_gatt_inspection(false, error->status, "service_discovery_failed");
    return 0;
}

int BleSensorGateway::gatt_characteristic_event(uint16_t conn_handle,
                                                const struct ble_gatt_error* error,
                                                const struct ble_gatt_chr* chr,
                                                void* arg) {
    auto* self = static_cast<BleSensorGateway*>(arg);
    if (!self || !error) {
        return 0;
    }
    if (error->status == 0 && chr) {
        BleGattCharacteristicInfo info;
        info.definition_handle = chr->def_handle;
        info.value_handle = chr->val_handle;
        info.properties = chr->properties;
        info.uuid = uuid_to_text(&chr->uuid.u);
        ScopedLock lock(self->lock_);
        if (self->gatt_service_index_ < self->gatt_inspection_.services.size()) {
            self->gatt_inspection_.services[self->gatt_service_index_].characteristics.push_back(info);
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        {
            ScopedLock lock(self->lock_);
            self->gatt_service_index_++;
        }
        self->start_next_gatt_characteristic_discovery();
        return 0;
    }
    {
        ScopedLock lock(self->lock_);
        self->gatt_service_index_++;
    }
    self->start_next_gatt_characteristic_discovery();
    return 0;
}

int BleSensorGateway::gatt_read_event(uint16_t conn_handle,
                                      const struct ble_gatt_error* error,
                                      struct ble_gatt_attr* attr,
                                      void* arg) {
    auto* self = static_cast<BleSensorGateway*>(arg);
    if (!self || !error) {
        return 0;
    }

    {
        ScopedLock lock(self->lock_);
        if (self->gatt_read_service_index_ < self->gatt_inspection_.services.size()) {
            auto& service = self->gatt_inspection_.services[self->gatt_read_service_index_];
            if (self->gatt_read_characteristic_index_ < service.characteristics.size()) {
                auto& chr = service.characteristics[self->gatt_read_characteristic_index_];
                chr.read_attempted = true;
                chr.read_error = error->status;
                chr.read_ok = error->status == 0;
                if (error->status == 0 && attr && attr->om) {
                    chr.value_hex = mbuf_to_hex(attr->om);
                }
            }
        }
        self->gatt_read_characteristic_index_++;
    }

    self->start_next_gatt_read();
    return 0;
}

void BleSensorGateway::start_next_gatt_characteristic_discovery() {
    uint16_t conn_handle = 0xffff;
    uint16_t start_handle = 0;
    uint16_t end_handle = 0;
    {
        ScopedLock lock(lock_);
        if (!gatt_inspection_.running || gatt_service_index_ >= gatt_inspection_.services.size()) {
            conn_handle = gatt_conn_handle_;
        } else {
            conn_handle = gatt_conn_handle_;
            start_handle = gatt_inspection_.services[gatt_service_index_].start_handle;
            end_handle = gatt_inspection_.services[gatt_service_index_].end_handle;
        }
    }

    if (start_handle == 0 || end_handle == 0 || conn_handle == 0xffff) {
        {
            ScopedLock lock(lock_);
            gatt_read_service_index_ = 0;
            gatt_read_characteristic_index_ = 0;
        }
        start_next_gatt_read();
        return;
    }

    int rc = ble_gattc_disc_all_chrs(conn_handle, start_handle, end_handle, gatt_characteristic_event, this);
    if (rc != 0) {
        {
            ScopedLock lock(lock_);
            gatt_service_index_++;
        }
        start_next_gatt_characteristic_discovery();
    }
}

void BleSensorGateway::start_next_gatt_read() {
    uint16_t conn_handle = 0xffff;
    uint16_t value_handle = 0;
    {
        ScopedLock lock(lock_);
        conn_handle = gatt_conn_handle_;
        while (gatt_read_service_index_ < gatt_inspection_.services.size()) {
            auto& service = gatt_inspection_.services[gatt_read_service_index_];
            while (gatt_read_characteristic_index_ < service.characteristics.size()) {
                auto& chr = service.characteristics[gatt_read_characteristic_index_];
                if ((chr.properties & BLE_GATT_CHR_F_READ) != 0) {
                    value_handle = chr.value_handle;
                    break;
                }
                gatt_read_characteristic_index_++;
            }
            if (value_handle != 0) {
                break;
            }
            gatt_read_service_index_++;
            gatt_read_characteristic_index_ = 0;
        }
    }

    if (conn_handle == 0xffff || value_handle == 0) {
        finish_gatt_inspection(true, 0, "");
        return;
    }

    int rc = ble_gattc_read(conn_handle, value_handle, gatt_read_event, this);
    if (rc != 0) {
        {
            ScopedLock lock(lock_);
            if (gatt_read_service_index_ < gatt_inspection_.services.size()) {
                auto& service = gatt_inspection_.services[gatt_read_service_index_];
                if (gatt_read_characteristic_index_ < service.characteristics.size()) {
                    auto& chr = service.characteristics[gatt_read_characteristic_index_];
                    chr.read_attempted = true;
                    chr.read_ok = false;
                    chr.read_error = rc;
                }
            }
            gatt_read_characteristic_index_++;
        }
        start_next_gatt_read();
    }
}

void BleSensorGateway::finish_gatt_inspection(bool ok, int error_code, const std::string& error) {
    uint16_t conn_handle = 0xffff;
    size_t service_count = 0;
    {
        ScopedLock lock(lock_);
        conn_handle = gatt_conn_handle_;
        gatt_inspection_.running = false;
        gatt_inspection_.ok = ok;
        gatt_inspection_.error_code = error_code;
        gatt_inspection_.error = error;
        gatt_inspection_.completed_at = uptime_seconds();
        gatt_inspection_.connected = false;
        service_count = gatt_inspection_.services.size();
        gatt_conn_handle_ = 0xffff;
        gatt_service_index_ = 0;
        gatt_read_service_index_ = 0;
        gatt_read_characteristic_index_ = 0;
    }

    if (conn_handle != 0xffff) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    char message[160];
    std::snprintf(message,
                  sizeof(message),
                  "BLE GATT inspect complete ok=%s services=%u err=%d %s",
                  ok ? "true" : "false",
                  static_cast<unsigned>(service_count),
                  error_code,
                  error.c_str());
    tiger_log(ok ? "INFO" : "WARN", TAG, message);
}

void BleSensorGateway::queue_advertisement(const ::ble_gap_disc_desc& disc) {
    PendingBleAdvertisement pending;
    std::copy(std::begin(disc.addr.val), std::end(disc.addr.val), pending.address.begin());
    pending.address_type = disc.addr.type;
    pending.rssi = disc.rssi;
    pending.last_seen = uptime_seconds();
    pending.data_len = static_cast<uint8_t>(std::min<size_t>(disc.length_data, pending.data.size()));
    if (pending.data_len > 0) {
        std::copy(disc.data, disc.data + pending.data_len, pending.data.begin());
    }

    ScopedLock lock(lock_);
    const size_t index = pending_write_ % pending_advertisements_.size();
    pending_advertisements_[index] = pending;
    pending_write_ = (pending_write_ + 1) % pending_advertisements_.size();
    if (pending_count_ < pending_advertisements_.size()) {
        pending_count_++;
    }
}

bool BleSensorGateway::pop_pending_advertisement(PendingBleAdvertisement& out) {
    ScopedLock lock(lock_);
    if (pending_count_ == 0) {
        return false;
    }
    const size_t read_index = (pending_write_ + pending_advertisements_.size() - pending_count_) % pending_advertisements_.size();
    out = pending_advertisements_[read_index];
    pending_count_--;
    return true;
}

void BleSensorGateway::process_pending_advertisements() {
    PendingBleAdvertisement pending;
    size_t processed = 0;
    while (processed < 24 && pop_pending_advertisement(pending)) {
        process_advertisement(pending);
        processed++;
    }
}

void BleSensorGateway::process_advertisement(const PendingBleAdvertisement& pending) {
    std::vector<uint8_t> raw(pending.data.begin(), pending.data.begin() + pending.data_len);
    BleAdvertisement adv;
    adv.mac_address = mac_to_string(pending.address);
    adv.rssi = pending.rssi;
    adv.last_seen = pending.last_seen;
    adv.raw = raw;
    adv.fields = parse_ble_ad_fields(raw.data(), raw.size());
    adv.name = ble_name_from_fields(adv.fields);
    ParsedBleSensorData parsed = parse_ble_sensor_advertisement(adv);

    BleSensorReading reading;
    reading.mac_address = adv.mac_address;
    reading.name = adv.name.empty() ? reading.mac_address : adv.name;
    reading.display_name = reading.name;
    reading.brand = parsed.brand;
    reading.model = parsed.model;
    reading.protocol = parsed.protocol;
    reading.parse_status = parsed.parse_status;
    reading.debug = parsed.debug;
    reading.rssi = pending.rssi;
    reading.last_seen = adv.last_seen;
    reading.raw_advertisement_hex = parsed.raw_adv_hex.empty() ? bytes_to_hex(raw) : parsed.raw_adv_hex;
    reading.has_temperature = parsed.has_temperature;
    reading.temperature_c = parsed.temperature_c;
    reading.has_humidity = parsed.has_humidity;
    reading.humidity_percent = parsed.humidity_percent;
    reading.has_battery = parsed.has_battery;
    reading.battery_percent = parsed.battery_percent;
    reading.has_external_probe_temperature = parsed.has_external_probe_temperature;
    reading.external_probe_temperature_c = parsed.external_probe_temperature_c;

    BleRawPacket packet;
    packet.mac_address = reading.mac_address;
    packet.address_type = address_type_text(pending.address_type);
    packet.name = reading.name;
    packet.brand = reading.brand;
    packet.model = reading.model;
    packet.protocol = reading.protocol;
    packet.parse_status = reading.parse_status;
    packet.debug = reading.debug;
    packet.rssi = reading.rssi;
    packet.last_seen = reading.last_seen;
    packet.raw_advertisement_hex = reading.raw_advertisement_hex;
    packet.has_temperature = reading.has_temperature;
    packet.temperature_c = reading.temperature_c;
    packet.has_humidity = reading.has_humidity;
    packet.humidity_percent = reading.humidity_percent;
    packet.has_battery = reading.has_battery;
    packet.battery_percent = reading.battery_percent;
    packet.has_external_probe_temperature = reading.has_external_probe_temperature;
    packet.external_probe_temperature_c = reading.external_probe_temperature_c;
    extract_ad_metadata(adv.fields, packet.manufacturer_data_hex, packet.service_data_hex, packet.service_uuids);

    const bool paired = is_paired(reading.mac_address);
    const bool relevant_packet = parsed.recognized || paired || has_readable_values(reading) ||
                                 has_known_sensor_service_data(adv.fields) || likely_sensor_name(adv.name);
    if (!relevant_packet) {
        ScopedLock lock(lock_);
        const size_t unknown_samples = static_cast<size_t>(std::count_if(raw_packets_.begin(), raw_packets_.end(), [](const auto& item) {
            return item.protocol == "unknown" && item.service_data_hex.empty() && item.service_uuids.empty();
        }));
        if (unknown_samples < MAX_UNKNOWN_RAW_SAMPLES && raw_packets_.size() < MAX_RAW_PACKETS) {
            raw_packets_.push_back(packet);
            last_packet_seen_ = packet.last_seen;
        }
        return;
    }

    remember_raw_packet(packet);

    upsert_reading(reading);
}

void BleSensorGateway::remember_raw_packet(const BleRawPacket& packet) {
    ScopedLock lock(lock_);
    if (raw_packets_.size() >= MAX_RAW_PACKETS) {
        raw_packets_.erase(raw_packets_.begin());
    }
    raw_packets_.push_back(packet);
    last_packet_seen_ = packet.last_seen;
}

void BleSensorGateway::upsert_reading(const BleSensorReading& reading) {
    auto pairs = nvs_store().get_ble_sensor_pairs();
    BleSensorReading next = reading;
    auto pair_it = std::find_if(pairs.begin(), pairs.end(), [&](const auto& cfg) {
        return cfg.mac_address == next.mac_address;
    });
    if (pair_it != pairs.end()) {
        next.paired = true;
        next.display_name = pair_it->name.empty() ? next.name : pair_it->name;
        next.location = pair_it->location;
        if (!pair_it->brand.empty() && pair_it->brand != "unknown") next.brand = pair_it->brand;
        if (!pair_it->model.empty() && pair_it->model != "unknown") next.model = pair_it->model;
        if (!pair_it->protocol.empty() && pair_it->protocol != "unknown" && pair_it->protocol != "ble_raw" && pair_it->protocol != "ble") {
            next.protocol = pair_it->protocol;
        }
    }

    {
        ScopedLock lock(lock_);
        auto it = std::find_if(discovered_.begin(), discovered_.end(), [&](const auto& item) {
            return item.mac_address == next.mac_address;
        });
        if (it == discovered_.end()) {
            if (discovered_.size() >= MAX_DISCOVERED) {
                discovered_.erase(std::min_element(discovered_.begin(), discovered_.end(), [](const auto& a, const auto& b) {
                    return a.last_seen < b.last_seen;
                }));
            }
            discovered_.push_back(next);
        } else {
            const bool existing_has_values = it->has_temperature || it->has_humidity || it->has_battery || it->has_external_probe_temperature;
            const bool next_has_values = next.has_temperature || next.has_humidity || next.has_battery || next.has_external_probe_temperature;
            if (existing_has_values && !next_has_values) {
                it->rssi = next.rssi;
                it->last_seen = next.last_seen;
                if (!next.name.empty() && next.name != next.mac_address) it->name = next.name;
                if (!next.raw_advertisement_hex.empty()) it->raw_advertisement_hex = next.raw_advertisement_hex;
                if (!next.debug.empty()) it->debug = next.debug;
                return;
            }
            if (next_has_values && !existing_has_values) {
                next.display_name = it->display_name.empty() ? next.display_name : it->display_name;
                next.location = it->location;
                next.paired = it->paired || next.paired;
            }
            *it = next;
        }
    }

    if (is_watched_ble_address(next.mac_address) && has_readable_values(next)) {
        auto cached = nvs_store().get_ble_sensor_last_readings();
        auto cache_it = std::find_if(cached.begin(), cached.end(), [&](const auto& item) {
            return item.mac_address == next.mac_address;
        });
        if (cache_it != cached.end() && next.last_seen < cache_it->last_seen + 30) {
            return;
        }
        BleSensorLastReading snapshot = to_last_reading(next);
        if (cache_it == cached.end()) {
            cached.push_back(snapshot);
        } else {
            *cache_it = snapshot;
        }
        if (cached.size() > 32) {
            cached.erase(cached.begin());
        }
        nvs_store().save_ble_sensor_last_readings(cached);
    }
}

bool BleSensorGateway::is_paired(const std::string& mac) {
    auto pairs = nvs_store().get_ble_sensor_pairs();
    return std::any_of(pairs.begin(), pairs.end(), [&](const auto& cfg) {
        return cfg.mac_address == mac;
    });
}

void BleSensorGateway::publish_due() {
    const uint64_t now = uptime_seconds();
    if (now - last_publish_ < MQTT_PUBLISH_SECONDS) {
        return;
    }
    last_publish_ = now;
    for (const auto& reading : paired_latest()) {
        if (reading.last_seen > 0) {
            publish_sensor(reading);
        }
    }
    for (const auto& device : device_registry().devices()) {
        if (device.online && device.last_seen > 0) {
            device_registry().publish_state(device);
        }
    }
}

void BleSensorGateway::publish_sensor(const BleSensorReading& reading) {
    auto status = device_manager().status();
    cJSON* root = cJSON_CreateObject();
    add_reading_json(root, reading, false);
    cJSON_AddStringToObject(root, "schema", "tigeros.ble_telemetry.v1");
    cJSON_AddStringToObject(root, "gateway_id", status.device_id.c_str());
    cJSON_AddStringToObject(root, "gateway_device_id", status.device_id.c_str());
    cJSON_AddStringToObject(root, "device_id", universal_device_id_from_address("ble_raw", reading.mac_address).c_str());
    cJSON_AddNumberToObject(root, "collected_at_uptime", static_cast<double>(reading.last_seen));
    cJSON_AddNumberToObject(root, "sent_at_uptime", static_cast<double>(status.uptime_seconds));
    cJSON_AddNumberToObject(root, "gateway_uptime", static_cast<double>(status.uptime_seconds));
    char* text = cJSON_PrintUnformatted(root);
    std::string topic = "tigeros/" + status.device_id + "/ble/" + reading.mac_address + "/telemetry";
    esp_err_t err = mqtt_manager().publish_raw(topic, text ? text : "{}", 0, false);
    cJSON_free(text);
    cJSON_Delete(root);
    if (err == ESP_OK) {
        tiger_log("INFO", TAG, "BLE sensor telemetry published");
    }
}

void BleSensorGateway::set_scanning(bool scanning) {
    scanning_ = scanning;
}

BleSensorGateway& ble_sensor_gateway() {
    static BleSensorGateway gateway;
    return gateway;
}

} // namespace tigeros
