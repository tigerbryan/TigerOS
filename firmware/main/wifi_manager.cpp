#include "wifi_manager.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "nvs_store.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "wifi_manager";
constexpr int SETUP_AP_DISCONNECT_THRESHOLD = 3;
} // namespace

esp_err_t WifiManager::init() {
    if (initialized_) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sta_netif_ = esp_netif_create_default_wifi_sta();
    ap_netif_ = esp_netif_create_default_wifi_ap();
    if (!sta_netif_ || !ap_netif_) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::event_handler, this, nullptr));

    event_group_ = xEventGroupCreate();
    initialized_ = true;
    return ESP_OK;
}

esp_err_t WifiManager::start() {
    auto credentials = nvs_store().get_wifi_credentials();
    if (credentials) {
        ESP_LOGI(TAG,
                 "Saved WiFi credentials found; setup AP will start after %d failed reconnects",
                 SETUP_AP_DISCONNECT_THRESHOLD);
        return connect_with_saved_credentials();
    }
    return start_setup_ap();
}

esp_err_t WifiManager::connect_with_saved_credentials() {
    auto credentials = nvs_store().get_wifi_credentials();
    if (!credentials) {
        return ESP_ERR_NOT_FOUND;
    }
    return configure_station(credentials->ssid, credentials->password);
}

esp_err_t WifiManager::save_and_connect(const std::string& ssid, const std::string& password) {
    ESP_RETURN_ON_ERROR(nvs_store().save_wifi_credentials(ssid, password), TAG, "save wifi credentials failed");
    return configure_station(ssid, password);
}

esp_err_t WifiManager::start_setup_ap() {
    if (ap_active_) {
        return ESP_OK;
    }

    ap_ssid_ = build_ap_ssid();

    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), ap_ssid_.c_str(), sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = ap_ssid_.size();
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch WiFi to APSTA mode: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure setup AP: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "Failed to start WiFi for setup AP: %s", esp_err_to_name(err));
        return err;
    }

    ap_active_ = true;
    if (!connected_) {
        ip_address_ = "192.168.4.1";
    }
    ESP_LOGI(TAG, "Setup AP available: %s at 192.168.4.1", ap_ssid_.c_str());
    return ESP_OK;
}

WifiStatus WifiManager::status() const {
    WifiStatus out;
    out.ap_active = ap_active_;
    out.sta_started = sta_started_;
    out.connected = connected_;
    out.ssid = current_ssid_;
    out.ap_ssid = ap_ssid_;
    out.ip_address = ip_address_;
    out.rssi = rssi_;

    if (connected_) {
        wifi_ap_record_t ap_record = {};
        if (esp_wifi_sta_get_ap_info(&ap_record) == ESP_OK) {
            out.rssi = ap_record.rssi;
        }
    }
    return out;
}

std::vector<WifiNetwork> WifiManager::scan_networks() {
    wifi_scan_config_t scan_config = {};
    scan_config.ssid = nullptr;
    scan_config.bssid = nullptr;
    scan_config.channel = 0;
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return {};
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&ap_count));
    if (ap_count == 0) {
        return {};
    }

    std::vector<wifi_ap_record_t> records(ap_count);
    err = esp_wifi_scan_get_ap_records(&ap_count, records.data());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read scan results: %s", esp_err_to_name(err));
        return {};
    }

    std::vector<WifiNetwork> networks;
    networks.reserve(ap_count);
    for (uint16_t i = 0; i < ap_count; ++i) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        WifiNetwork network;
        network.ssid = reinterpret_cast<const char*>(records[i].ssid);
        network.rssi = records[i].rssi;
        network.channel = records[i].primary;
        network.auth_mode = auth_mode_name(records[i].authmode);
        networks.push_back(network);
    }
    return networks;
}

void WifiManager::event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static_cast<WifiManager*>(arg)->handle_event(event_base, event_id, event_data);
}

void WifiManager::handle_event(esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        sta_started_ = true;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        connected_ = false;
        ip_address_.clear();
        disconnect_count_ += 1;
        if (!ap_active_ && disconnect_count_ >= SETUP_AP_DISCONNECT_THRESHOLD) {
            ESP_LOGW(TAG, "WiFi disconnected %d times, starting setup AP", disconnect_count_);
            ESP_ERROR_CHECK_WITHOUT_ABORT(start_setup_ap());
        } else {
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
        }
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ap_active_ = true;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        ap_active_ = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        connected_ = true;
        disconnect_count_ = 0;
        update_ip_address(event->ip_info);
        ESP_LOGI(TAG, "Connected with IP: %s", ip_address_.c_str());
        if (ap_active_) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(stop_setup_ap());
        }
    }
}

esp_err_t WifiManager::configure_station(const std::string& ssid, const std::string& password) {
    wifi_config_t sta_config = {};
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.ssid), ssid.c_str(), sizeof(sta_config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.password), password.c_str(), sizeof(sta_config.sta.password));
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    current_ssid_ = ssid;
    sta_started_ = true;
    disconnect_count_ = 0;

    wifi_mode_t mode = ap_active_ ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        return err;
    }
    return esp_wifi_connect();
}

esp_err_t WifiManager::stop_setup_ap() {
    if (!ap_active_) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_set_mode(sta_started_ ? WIFI_MODE_STA : WIFI_MODE_NULL);
    if (err != ESP_OK) {
        return err;
    }
    ap_active_ = false;
    ap_ssid_.clear();
    ESP_LOGI(TAG, "Setup AP stopped");
    return ESP_OK;
}

std::string WifiManager::build_ap_ssid() const {
    std::array<uint8_t, 6> mac{};
    ESP_ERROR_CHECK(esp_read_mac(mac.data(), ESP_MAC_WIFI_SOFTAP));
    char ssid[32];
    std::snprintf(ssid, sizeof(ssid), "TigerOS-Setup-%02X%02X", mac[4], mac[5]);
    return ssid;
}

void WifiManager::update_ip_address(esp_netif_ip_info_t ip_info) {
    char ip[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    ip_address_ = ip;
}

std::string WifiManager::auth_mode_name(wifi_auth_mode_t auth_mode) {
    switch (auth_mode) {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
            return "wpa";
        case WIFI_AUTH_WPA2_PSK:
            return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "wpa2-enterprise";
        case WIFI_AUTH_WPA3_PSK:
            return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "wpa2/wpa3";
        default:
            return "unknown";
    }
}

WifiManager& wifi_manager() {
    static WifiManager manager;
    return manager;
}

} // namespace tigeros
