#pragma once

#include <string>

#include "esp_err.h"
#include "nvs_store.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace tigeros {

enum class BleProvisioningState {
    Disabled,
    Starting,
    Advertising,
    Connected,
    Provisioning,
    Provisioned,
    Error,
};

struct BleStatus {
    bool enabled = true;
    bool connected = false;
    std::string device_name;
    std::string state;
    std::string provisioning_state;
    bool pop_required = false;
    std::string pairing_pin;
};

class BleManager {
public:
    esp_err_t init();
    esp_err_t apply_config(const BleConfig& config);
    BleConfig config();
    BleStatus status();
    bool provisioning_advertising() const;
    void stop_provisioning_for_scan();
    std::string device_info_json();
    std::string status_json();
    void handle_provisioning_payload(const std::string& payload);

    static void host_task(void* param);
    static void on_reset(int reason);
    static void on_sync();
    static int gap_event(::ble_gap_event* event, void* arg);
    static int gatt_access(uint16_t conn_handle, uint16_t attr_handle, ::ble_gatt_access_ctxt* ctxt, void* arg);
    static void provision_task(void* arg);

private:
    esp_err_t init_host();
    void start_advertising();
    void stop_advertising();
    void set_state(BleProvisioningState state);
    std::string build_device_name();

    bool initialized_ = false;
    bool host_started_ = false;
    bool connected_ = false;
    uint8_t own_addr_type_ = 0;
    BleProvisioningState state_ = BleProvisioningState::Disabled;
    BleConfig config_;
    std::string device_name_;
    std::string last_error_;
};

BleManager& ble_manager();

} // namespace tigeros
