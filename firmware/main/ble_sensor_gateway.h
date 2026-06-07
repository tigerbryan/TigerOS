#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ble_sensor_parser.h"
#include "nvs_store.h"

struct ble_gap_event;
struct ble_gap_disc_desc;
struct ble_gatt_error;
struct ble_gatt_svc;
struct ble_gatt_chr;
struct ble_gatt_attr;

namespace tigeros {

struct BleSensorReading {
    std::string mac_address;
    std::string name;
    std::string display_name;
    std::string brand = "unknown";
    std::string model = "unknown";
    std::string protocol = "unknown";
    std::string parse_status = "unknown";
    std::string location;
    std::string debug;
    int rssi = 0;
    uint64_t last_seen = 0;
    std::string raw_advertisement_hex;
    bool paired = false;
    bool has_temperature = false;
    bool has_humidity = false;
    bool has_battery = false;
    bool has_external_probe_temperature = false;
    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
    int battery_percent = 0;
    float external_probe_temperature_c = 0.0f;
};

struct BleRawPacket {
    std::string mac_address;
    std::string address_type;
    std::string name;
    std::string brand = "unknown";
    std::string model = "unknown";
    std::string protocol = "unknown";
    std::string parse_status = "unknown";
    std::string debug;
    int rssi = 0;
    uint64_t last_seen = 0;
    std::string raw_advertisement_hex;
    std::string manufacturer_data_hex;
    std::string service_data_hex;
    std::vector<std::string> service_uuids;
    bool has_temperature = false;
    bool has_humidity = false;
    bool has_battery = false;
    bool has_external_probe_temperature = false;
    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
    int battery_percent = 0;
    float external_probe_temperature_c = 0.0f;
};

struct BleScannerStats {
    bool scanning = false;
    bool scan_requested = false;
    bool provisioning_advertising = false;
    size_t raw_packet_count = 0;
    size_t discovered_device_count = 0;
    size_t paired_device_count = 0;
    size_t readable_device_count = 0;
    size_t bthome_packet_count = 0;
    size_t unknown_packet_count = 0;
    uint64_t last_scan_started = 0;
    uint64_t last_packet_seen = 0;
};

struct BleGattCharacteristicInfo {
    uint16_t definition_handle = 0;
    uint16_t value_handle = 0;
    uint8_t properties = 0;
    std::string uuid;
    bool read_attempted = false;
    bool read_ok = false;
    int read_error = 0;
    std::string value_hex;
};

struct BleGattServiceInfo {
    uint16_t start_handle = 0;
    uint16_t end_handle = 0;
    std::string uuid;
    std::vector<BleGattCharacteristicInfo> characteristics;
};

struct BleGattInspection {
    bool running = false;
    bool connected = false;
    bool ok = false;
    int error_code = 0;
    std::string error;
    std::string mac_address;
    std::string address_type;
    uint64_t started_at = 0;
    uint64_t completed_at = 0;
    std::vector<BleGattServiceInfo> services;
};

struct PendingBleAdvertisement {
    std::array<uint8_t, 6> address{};
    uint8_t address_type = 0;
    int rssi = 0;
    uint64_t last_seen = 0;
    uint8_t data_len = 0;
    std::array<uint8_t, 64> data{};
};

class BleSensorGateway {
public:
    esp_err_t init();
    esp_err_t request_scan();
    esp_err_t start_scan();
    esp_err_t stop_scan();
    void pause_auto_scan(const char* reason);
    void resume_auto_scan();
    bool scanning() const;
    std::vector<BleSensorReading> discovered();
    std::vector<BleSensorReading> paired_latest();
    std::vector<PairedBleSensorConfig> paired_configs();
    esp_err_t pair_sensor(const PairedBleSensorConfig& config);
    esp_err_t pair_sensor(const std::string& mac, const std::string& name, const std::string& type);
    esp_err_t remove_sensor(const std::string& mac);
    esp_err_t rename_sensor(const std::string& mac, const std::string& name);
    esp_err_t set_location(const std::string& mac, const std::string& location);
    esp_err_t set_bindkey(const std::string& mac, const std::string& bindkey);
    std::vector<BleSensorReading> raw_packets();
    std::vector<BleRawPacket> raw_packet_history();
    BleScannerStats stats();
    void clear_raw_packets();
    size_t discovered_count() const;
    size_t paired_count();
    esp_err_t inspect_gatt(const std::string& mac);
    BleGattInspection gatt_inspection();

    static int gap_event(::ble_gap_event* event, void* arg);
    static int gatt_service_event(uint16_t conn_handle,
                                  const ::ble_gatt_error* error,
                                  const ::ble_gatt_svc* service,
                                  void* arg);
    static int gatt_characteristic_event(uint16_t conn_handle,
                                         const ::ble_gatt_error* error,
                                         const ::ble_gatt_chr* chr,
                                         void* arg);
    static int gatt_read_event(uint16_t conn_handle,
                               const ::ble_gatt_error* error,
                               ::ble_gatt_attr* attr,
                               void* arg);

private:
    static void task_entry(void* arg);
    void loop();
    void handle_gap_event(::ble_gap_event* event);
    void queue_advertisement(const ::ble_gap_disc_desc& disc);
    bool pop_pending_advertisement(PendingBleAdvertisement& out);
    void process_pending_advertisements();
    void process_advertisement(const PendingBleAdvertisement& pending);
    esp_err_t start_scan_window(uint32_t window_ms, bool active_scan);
    void remember_raw_packet(const BleRawPacket& packet);
    void upsert_reading(const BleSensorReading& reading);
    bool is_paired(const std::string& mac);
    void publish_due();
    void publish_sensor(const BleSensorReading& reading);
    void set_scanning(bool value);
    void start_next_gatt_characteristic_discovery();
    void start_next_gatt_read();
    void finish_gatt_inspection(bool ok, int error_code, const std::string& error);

    bool initialized_ = false;
    bool scanning_ = false;
    bool scan_requested_ = false;
    bool manual_scan_pending_ = false;
    bool auto_scan_paused_ = false;
    bool task_started_ = false;
    uint8_t own_addr_type_ = 0;
    uint64_t last_scan_started_ = 0;
    uint64_t last_packet_seen_ = 0;
    uint64_t last_publish_ = 0;
    SemaphoreHandle_t lock_ = nullptr;
    std::vector<BleSensorReading> discovered_;
    std::vector<BleRawPacket> raw_packets_;
    std::array<PendingBleAdvertisement, 96> pending_advertisements_{};
    size_t pending_write_ = 0;
    size_t pending_count_ = 0;
    BleGattInspection gatt_inspection_;
    uint16_t gatt_conn_handle_ = 0xffff;
    size_t gatt_service_index_ = 0;
    size_t gatt_read_service_index_ = 0;
    size_t gatt_read_characteristic_index_ = 0;
};

BleSensorGateway& ble_sensor_gateway();

} // namespace tigeros
