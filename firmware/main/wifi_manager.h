#pragma once

#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace tigeros {

enum class WifiMode {
    SetupAp,
    Station,
    ApSta,
};

struct WifiStatus {
    bool ap_active = false;
    bool sta_started = false;
    bool connected = false;
    std::string ssid;
    std::string ap_ssid;
    std::string ip_address;
    int rssi = 0;
};

struct WifiNetwork {
    std::string ssid;
    int rssi = 0;
    int channel = 0;
    std::string auth_mode;
};

class WifiManager {
public:
    esp_err_t init();
    esp_err_t start();
    esp_err_t connect_with_saved_credentials();
    esp_err_t save_and_connect(const std::string& ssid, const std::string& password);
    esp_err_t start_setup_ap();
    WifiStatus status() const;
    std::vector<WifiNetwork> scan_networks();

private:
    static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    void handle_event(esp_event_base_t event_base, int32_t event_id, void* event_data);
    esp_err_t configure_station(const std::string& ssid, const std::string& password);
    std::string build_ap_ssid() const;
    void update_ip_address(esp_netif_ip_info_t ip_info);
    static std::string auth_mode_name(wifi_auth_mode_t auth_mode);

    esp_netif_t* sta_netif_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;
    bool initialized_ = false;
    bool ap_active_ = false;
    bool sta_started_ = false;
    bool connected_ = false;
    std::string current_ssid_;
    std::string ap_ssid_;
    std::string ip_address_;
    int rssi_ = 0;
};

WifiManager& wifi_manager();

} // namespace tigeros
