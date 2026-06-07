#include "ble_manager.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "cJSON.h"
#include "device_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#include "tiger_log.h"
#include "wifi_manager.h"

extern "C" void ble_store_config_init(void);

namespace tigeros {
namespace {

constexpr const char* TAG = "ble_manager";

enum CharacteristicId {
    CHAR_DEVICE_INFO = 1,
    CHAR_PROVISIONING = 2,
    CHAR_PROVISIONING_STATUS = 3,
    CHAR_STATUS = 4,
};

BleManager* g_manager = nullptr;

static const ble_uuid128_t UUID_DEVICE_SERVICE =
    BLE_UUID128_INIT(0x01, 0x10, 0x00, 0xF0, 0x4F, 0x53, 0x4F, 0x72, 0x65, 0x67, 0x69, 0x54, 0x00, 0x00, 0x00, 0x01);
static const ble_uuid128_t UUID_DEVICE_INFO =
    BLE_UUID128_INIT(0x02, 0x10, 0x00, 0xF0, 0x4F, 0x53, 0x4F, 0x72, 0x65, 0x67, 0x69, 0x54, 0x00, 0x00, 0x00, 0x01);
static const ble_uuid128_t UUID_PROV_SERVICE =
    BLE_UUID128_INIT(0x01, 0x20, 0x00, 0xF0, 0x4F, 0x53, 0x4F, 0x72, 0x65, 0x67, 0x69, 0x54, 0x00, 0x00, 0x00, 0x01);
static const ble_uuid128_t UUID_PROV_WRITE =
    BLE_UUID128_INIT(0x02, 0x20, 0x00, 0xF0, 0x4F, 0x53, 0x4F, 0x72, 0x65, 0x67, 0x69, 0x54, 0x00, 0x00, 0x00, 0x01);
static const ble_uuid128_t UUID_PROV_STATUS =
    BLE_UUID128_INIT(0x03, 0x20, 0x00, 0xF0, 0x4F, 0x53, 0x4F, 0x72, 0x65, 0x67, 0x69, 0x54, 0x00, 0x00, 0x00, 0x01);
static const ble_uuid128_t UUID_STATUS_SERVICE =
    BLE_UUID128_INIT(0x01, 0x30, 0x00, 0xF0, 0x4F, 0x53, 0x4F, 0x72, 0x65, 0x67, 0x69, 0x54, 0x00, 0x00, 0x00, 0x01);
static const ble_uuid128_t UUID_STATUS =
    BLE_UUID128_INIT(0x02, 0x30, 0x00, 0xF0, 0x4F, 0x53, 0x4F, 0x72, 0x65, 0x67, 0x69, 0x54, 0x00, 0x00, 0x00, 0x01);

const char* state_text(BleProvisioningState state) {
    switch (state) {
        case BleProvisioningState::Starting:
            return "Starting";
        case BleProvisioningState::Advertising:
            return "Advertising";
        case BleProvisioningState::Connected:
            return "Connected";
        case BleProvisioningState::Provisioning:
            return "Provisioning";
        case BleProvisioningState::Provisioned:
            return "Provisioned";
        case BleProvisioningState::Error:
            return "Error";
        default:
            return "Disabled";
    }
}

std::string read_mbuf(struct os_mbuf* om) {
    const uint16_t len = OS_MBUF_PKTLEN(om);
    std::string out(len, '\0');
    if (len == 0) {
        return {};
    }
    if (ble_hs_mbuf_to_flat(om, out.data(), out.size(), nullptr) != 0) {
        return {};
    }
    return out;
}

int append_string(struct os_mbuf* om, const std::string& value) {
    return os_mbuf_append(om, value.data(), value.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

struct ProvisionRequest {
    std::string ssid;
    std::string password;
    std::string pop;
};

static const struct ble_gatt_chr_def device_characteristics[] = {
    {
        .uuid = &UUID_DEVICE_INFO.u,
        .access_cb = BleManager::gatt_access,
        .arg = reinterpret_cast<void*>(CHAR_DEVICE_INFO),
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ,
        .min_key_size = 0,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {}
};

static const struct ble_gatt_chr_def provisioning_characteristics[] = {
    {
        .uuid = &UUID_PROV_WRITE.u,
        .access_cb = BleManager::gatt_access,
        .arg = reinterpret_cast<void*>(CHAR_PROVISIONING),
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
        .min_key_size = 0,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {
        .uuid = &UUID_PROV_STATUS.u,
        .access_cb = BleManager::gatt_access,
        .arg = reinterpret_cast<void*>(CHAR_PROVISIONING_STATUS),
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {}
};

static const struct ble_gatt_chr_def status_characteristics[] = {
    {
        .uuid = &UUID_STATUS.u,
        .access_cb = BleManager::gatt_access,
        .arg = reinterpret_cast<void*>(CHAR_STATUS),
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {}
};

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_DEVICE_SERVICE.u,
        .includes = nullptr,
        .characteristics = device_characteristics,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_PROV_SERVICE.u,
        .includes = nullptr,
        .characteristics = provisioning_characteristics,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_STATUS_SERVICE.u,
        .includes = nullptr,
        .characteristics = status_characteristics,
    },
    {},
};

} // namespace

esp_err_t BleManager::init() {
    if (initialized_) {
        return ESP_OK;
    }
    config_ = nvs_store().get_ble_config();
    device_name_ = build_device_name();
    g_manager = this;
    initialized_ = true;
    return init_host();
}

esp_err_t BleManager::apply_config(const BleConfig& config) {
    ESP_RETURN_ON_ERROR(nvs_store().save_ble_config(config), TAG, "save ble config");
    config_ = nvs_store().get_ble_config();
    if (!config_.enabled) {
        stop_advertising();
        connected_ = false;
        set_state(BleProvisioningState::Disabled);
    } else if (host_started_) {
        start_advertising();
    }
    tiger_log("INFO", TAG, config_.enabled ? "BLE provisioning enabled" : "BLE provisioning disabled");
    return ESP_OK;
}

BleConfig BleManager::config() {
    return config_;
}

BleStatus BleManager::status() {
    BleStatus out;
    out.enabled = config_.enabled;
    out.connected = connected_;
    out.device_name = device_name_;
    out.state = state_text(state_);
    out.provisioning_state = state_text(state_);
    out.pop_required = !config_.pop_token.empty();
    out.pairing_pin = config_.pairing_pin;
    return out;
}

bool BleManager::provisioning_advertising() const {
    return state_ == BleProvisioningState::Advertising;
}

void BleManager::stop_provisioning_for_scan() {
    if (state_ == BleProvisioningState::Advertising) {
        stop_advertising();
        set_state(BleProvisioningState::Disabled);
        tiger_log("INFO", TAG, "BLE provisioning advertising stopped for sensor scan");
    }
}

std::string BleManager::device_info_json() {
    auto status = device_manager().status();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", status.device_id.c_str());
    cJSON_AddStringToObject(root, "firmware", status.firmware_version.c_str());
    cJSON_AddStringToObject(root, "build_time", status.build_time.c_str());
    cJSON_AddStringToObject(root, "device_name", device_name_.c_str());
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text;
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

std::string BleManager::status_json() {
    auto ble = status();
    auto wifi = wifi_manager().status();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", ble.enabled);
    cJSON_AddBoolToObject(root, "connected", ble.connected);
    cJSON_AddStringToObject(root, "device_name", ble.device_name.c_str());
    cJSON_AddStringToObject(root, "state", ble.state.c_str());
    cJSON_AddStringToObject(root, "provisioning_state", ble.provisioning_state.c_str());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi.connected);
    cJSON_AddStringToObject(root, "ip", wifi.ip_address.c_str());
    cJSON_AddBoolToObject(root, "pop_required", ble.pop_required);
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text;
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

void BleManager::handle_provisioning_payload(const std::string& payload) {
    tiger_log("INFO", TAG, "BLE provisioning payload received");
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        last_error_ = "Invalid JSON";
        set_state(BleProvisioningState::Error);
        return;
    }

    cJSON* ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON* password = cJSON_GetObjectItem(root, "password");
    cJSON* pop = cJSON_GetObjectItem(root, "pop");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        last_error_ = "SSID is required";
        set_state(BleProvisioningState::Error);
        return;
    }
    if (!config_.pop_token.empty() && (!cJSON_IsString(pop) || config_.pop_token != pop->valuestring)) {
        cJSON_Delete(root);
        last_error_ = "Invalid proof-of-possession token";
        set_state(BleProvisioningState::Error);
        tiger_log("WARN", TAG, "BLE provisioning rejected by PoP");
        return;
    }

    auto* req = new ProvisionRequest;
    req->ssid = ssid->valuestring;
    req->password = cJSON_IsString(password) ? password->valuestring : "";
    req->pop = cJSON_IsString(pop) ? pop->valuestring : "";
    cJSON_Delete(root);

    set_state(BleProvisioningState::Provisioning);
    xTaskCreate(provision_task, "ble_provision", 4096, req, 5, nullptr);
}

esp_err_t BleManager::init_host() {
    set_state(config_.enabled ? BleProvisioningState::Starting : BleProvisioningState::Disabled);
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        last_error_ = "nimble_port_init failed";
        set_state(BleProvisioningState::Error);
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(gatt_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(gatt_services);
    }
    if (rc != 0) {
        set_state(BleProvisioningState::Error);
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(device_name_.c_str());
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    host_started_ = true;
    tiger_log("INFO", TAG, "BLE provisioning host started");
    return ESP_OK;
}

void BleManager::host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void BleManager::on_reset(int reason) {
    if (g_manager) {
        g_manager->last_error_ = "BLE reset";
        g_manager->set_state(BleProvisioningState::Error);
    }
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

void BleManager::on_sync() {
    if (!g_manager) return;
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &g_manager->own_addr_type_) != 0) {
        g_manager->set_state(BleProvisioningState::Error);
        return;
    }
    if (g_manager->config_.enabled && !wifi_manager().status().connected) {
        g_manager->start_advertising();
    } else if (g_manager->config_.enabled) {
        g_manager->set_state(BleProvisioningState::Disabled);
        tiger_log("INFO", TAG, "BLE provisioning idle because WiFi is connected");
    }
}

void BleManager::start_advertising() {
    if (!config_.enabled) {
        set_state(BleProvisioningState::Disabled);
        return;
    }
    if (wifi_manager().status().connected) {
        stop_advertising();
        set_state(BleProvisioningState::Disabled);
        tiger_log("INFO", TAG, "BLE provisioning advertising skipped; WiFi is connected");
        return;
    }

    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<const uint8_t*>(device_name_.c_str());
    fields.name_len = device_name_.size();
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(own_addr_type_, nullptr, BLE_HS_FOREVER, &params, gap_event, this);
    if (rc == 0) {
        set_state(BleProvisioningState::Advertising);
        tiger_log("INFO", TAG, "BLE advertising started");
    } else {
        set_state(BleProvisioningState::Error);
        tiger_log("ERROR", TAG, "BLE advertising failed");
    }
}

void BleManager::stop_advertising() {
    ble_gap_adv_stop();
}

int BleManager::gap_event(struct ble_gap_event* event, void* arg) {
    auto* self = static_cast<BleManager*>(arg);
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            self->connected_ = event->connect.status == 0;
            self->set_state(self->connected_ ? BleProvisioningState::Connected : BleProvisioningState::Advertising);
            if (event->connect.status != 0) self->start_advertising();
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            self->connected_ = false;
            if (self->config_.enabled && !wifi_manager().status().connected) self->start_advertising();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (self->config_.enabled && !wifi_manager().status().connected) self->start_advertising();
            break;
        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            struct ble_sm_io pkey = {};
            pkey.action = event->passkey.params.action;
            pkey.passkey = std::strtoul(self->config_.pairing_pin.c_str(), nullptr, 10);
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            tiger_log("INFO", TAG, "BLE pairing PIN requested");
            break;
        }
        default:
            break;
    }
    return 0;
}

int BleManager::gatt_access(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    if (!g_manager) return BLE_ATT_ERR_UNLIKELY;
    const auto id = static_cast<CharacteristicId>(reinterpret_cast<intptr_t>(arg));
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (id == CHAR_DEVICE_INFO) return append_string(ctxt->om, g_manager->device_info_json());
        if (id == CHAR_PROVISIONING_STATUS || id == CHAR_STATUS) return append_string(ctxt->om, g_manager->status_json());
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && id == CHAR_PROVISIONING) {
        g_manager->handle_provisioning_payload(read_mbuf(ctxt->om));
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

void BleManager::provision_task(void* arg) {
    std::unique_ptr<ProvisionRequest> req(static_cast<ProvisionRequest*>(arg));
    esp_err_t err = wifi_manager().save_and_connect(req->ssid, req->password);
    if (g_manager) {
        if (err == ESP_OK) {
            g_manager->last_error_.clear();
            g_manager->set_state(BleProvisioningState::Provisioned);
            tiger_log("INFO", TAG, "BLE WiFi credentials saved");
        } else {
            g_manager->last_error_ = "Failed to save WiFi credentials";
            g_manager->set_state(BleProvisioningState::Error);
            tiger_log("ERROR", TAG, "BLE WiFi provisioning failed");
        }
    }
    vTaskDelete(nullptr);
}

void BleManager::set_state(BleProvisioningState state) {
    state_ = state;
}

std::string BleManager::build_device_name() {
    std::array<uint8_t, 6> mac{};
    esp_read_mac(mac.data(), ESP_MAC_BT);
    char name[24];
    std::snprintf(name, sizeof(name), "TigerOS-%02X%02X", mac[4], mac[5]);
    return name;
}

BleManager& ble_manager() {
    static BleManager manager;
    return manager;
}

} // namespace tigeros
