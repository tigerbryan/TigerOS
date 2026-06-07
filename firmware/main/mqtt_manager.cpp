#include "mqtt_manager.h"

#include "ble_sensor_gateway.h"
#include "cJSON.h"
#include "device_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_store.h"
#include "ota_manager.h"
#include "tiger_log.h"
#include "wifi_manager.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "mqtt_manager";

uint64_t uptime_seconds() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
}

void delayed_restart_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
}

void schedule_restart() {
    xTaskCreate(delayed_restart_task, "mqtt_restart", 2048, nullptr, 5, nullptr);
}

} // namespace

esp_err_t MqttManager::init() {
    if (initialized_) {
        return ESP_OK;
    }
    config_ = nvs_store().get_mqtt_config();
    state_ = config_.enabled ? MqttConnectionState::Disconnected : MqttConnectionState::Disabled;
    initialized_ = true;
    if (!task_started_) {
        xTaskCreate(task_entry, "mqtt_manager", 8192, this, 5, nullptr);
        task_started_ = true;
    }
    return ESP_OK;
}

esp_err_t MqttManager::apply_config(const MqttConfig& config) {
    ESP_RETURN_ON_ERROR(nvs_store().save_mqtt_config(config), TAG, "save mqtt config");
    stop_client();
    config_ = nvs_store().get_mqtt_config();
    set_state(config_.enabled ? MqttConnectionState::Disconnected : MqttConnectionState::Disabled);
    tiger_log("INFO", TAG, config_.enabled ? "MQTT settings saved; reconnect scheduled" : "MQTT disabled");
    return ESP_OK;
}

MqttConfig MqttManager::config() {
    return config_;
}

MqttStatus MqttManager::status() {
    MqttStatus out;
    out.enabled = config_.enabled;
    out.state = state_;
    out.host = config_.host;
    out.port = config_.port;
    out.client_id = config_.client_id;
    out.use_tls = config_.use_tls;
    switch (state_) {
        case MqttConnectionState::Disabled:
            out.state_text = "Disabled";
            break;
        case MqttConnectionState::Connecting:
            out.state_text = "Connecting";
            break;
        case MqttConnectionState::Connected:
            out.state_text = "Connected";
            break;
        case MqttConnectionState::Error:
            out.state_text = "Error";
            break;
        default:
            out.state_text = "Disconnected";
            break;
    }
    return out;
}

esp_err_t MqttManager::publish_status(bool online) {
    if (!client_ || state_ != MqttConnectionState::Connected) {
        return ESP_ERR_INVALID_STATE;
    }

    auto status = device_manager().status();
    auto wifi = wifi_manager().status();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", status.device_id.c_str());
    cJSON_AddStringToObject(root, "firmware", status.firmware_version.c_str());
    cJSON_AddStringToObject(root, "build_time", status.build_time.c_str());
    cJSON_AddStringToObject(root, "wifi_ssid", status.wifi_ssid.c_str());
    cJSON_AddStringToObject(root, "ip", status.ip_address.c_str());
    cJSON_AddNumberToObject(root, "uptime", static_cast<double>(status.uptime_seconds));
    cJSON_AddNumberToObject(root, "free_heap", status.free_heap);
    cJSON_AddNumberToObject(root, "rssi", wifi.rssi);
    cJSON_AddBoolToObject(root, "online", online);
    char* text = cJSON_PrintUnformatted(root);
    const int msg_id = esp_mqtt_client_publish(client_, topic("status").c_str(), text, 0, 1, 1);
    cJSON_free(text);
    cJSON_Delete(root);
    if (msg_id < 0) {
        tiger_log("WARN", TAG, "MQTT status publish failed");
        return ESP_FAIL;
    }
    tiger_log("INFO", TAG, "MQTT status published");
    return ESP_OK;
}

esp_err_t MqttManager::publish_log(const std::string& level, const std::string& tag_name, const std::string& message) {
    if (!client_ || state_ != MqttConnectionState::Connected) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime", static_cast<double>(uptime_seconds()));
    cJSON_AddStringToObject(root, "level", level.c_str());
    cJSON_AddStringToObject(root, "tag", tag_name.c_str());
    cJSON_AddStringToObject(root, "message", message.c_str());
    char* text = cJSON_PrintUnformatted(root);
    const int msg_id = esp_mqtt_client_publish(client_, topic("log").c_str(), text, 0, 0, 0);
    cJSON_free(text);
    cJSON_Delete(root);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t MqttManager::publish_raw(const std::string& publish_topic, const std::string& payload, int qos, bool retain) {
    if (!client_ || state_ != MqttConnectionState::Connected || publish_topic.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    const int msg_id = esp_mqtt_client_publish(
        client_,
        publish_topic.c_str(),
        payload.c_str(),
        payload.size(),
        qos,
        retain ? 1 : 0
    );
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t MqttManager::publish_home_assistant_discovery() {
    if (!client_ || state_ != MqttConnectionState::Connected || !config_.ha_discovery_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    const std::string device_id = nvs_store().get_or_create_device_id();
    const std::string status_topic = topic("status");
    const std::string command_topic = topic("command");
    auto status = device_manager().status();

    auto add_device = [&](cJSON* root) {
        cJSON* device = cJSON_AddObjectToObject(root, "device");
        cJSON* identifiers = cJSON_AddArrayToObject(device, "identifiers");
        cJSON_AddItemToArray(identifiers, cJSON_CreateString(device_id.c_str()));
        cJSON_AddStringToObject(device, "name", device_id.c_str());
        cJSON_AddStringToObject(device, "manufacturer", "TigerOS");
        cJSON_AddStringToObject(device, "model", "ESP32-S3");
        cJSON_AddStringToObject(device, "sw_version", status.firmware_version.c_str());
    };
    auto add_availability = [&](cJSON* root) {
        cJSON_AddStringToObject(root, "availability_topic", status_topic.c_str());
        cJSON_AddStringToObject(root, "availability_template", "{{ 'online' if value_json.online else 'offline' }}");
        cJSON_AddStringToObject(root, "payload_available", "online");
        cJSON_AddStringToObject(root, "payload_not_available", "offline");
    };
    auto add_sensor = [&](const char* object_suffix, const char* name, const char* unique_suffix,
                          const char* value_template, const char* device_class,
                          const char* state_class, const char* unit) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", name);
        cJSON_AddStringToObject(root, "unique_id", (device_id + "_" + unique_suffix).c_str());
        cJSON_AddStringToObject(root, "object_id", (device_id + "_" + unique_suffix).c_str());
        cJSON_AddStringToObject(root, "state_topic", status_topic.c_str());
        cJSON_AddStringToObject(root, "value_template", value_template);
        if (device_class) cJSON_AddStringToObject(root, "device_class", device_class);
        if (state_class) cJSON_AddStringToObject(root, "state_class", state_class);
        if (unit) cJSON_AddStringToObject(root, "unit_of_measurement", unit);
        add_availability(root);
        add_device(root);
        esp_err_t err = publish_ha_entity("sensor", object_suffix, root);
        cJSON_Delete(root);
        return err;
    };

    esp_err_t err = ESP_OK;
    if (add_sensor("rssi", "TigerOS RSSI", "rssi", "{{ value_json.rssi }}", "signal_strength", "measurement", "dBm") != ESP_OK) err = ESP_FAIL;
    if (add_sensor("heap", "TigerOS Free Heap", "free_heap", "{{ value_json.free_heap }}", "data_size", "measurement", "B") != ESP_OK) err = ESP_FAIL;
    if (add_sensor("uptime", "TigerOS Uptime", "uptime", "{{ value_json.uptime }}", "duration", "total_increasing", "s") != ESP_OK) err = ESP_FAIL;
    if (add_sensor("firmware", "TigerOS Firmware", "firmware", "{{ value_json.firmware }}", nullptr, nullptr, nullptr) != ESP_OK) err = ESP_FAIL;

    cJSON* button = cJSON_CreateObject();
    cJSON_AddStringToObject(button, "name", "TigerOS Restart Device");
    cJSON_AddStringToObject(button, "unique_id", (device_id + "_restart").c_str());
    cJSON_AddStringToObject(button, "object_id", (device_id + "_restart").c_str());
    cJSON_AddStringToObject(button, "command_topic", command_topic.c_str());
    cJSON_AddStringToObject(button, "payload_press", "{\"command\":\"reboot\"}");
    cJSON_AddStringToObject(button, "device_class", "restart");
    add_availability(button);
    add_device(button);
    if (publish_ha_entity("button", "restart", button) != ESP_OK) err = ESP_FAIL;
    cJSON_Delete(button);

    cJSON* sw = cJSON_CreateObject();
    cJSON_AddStringToObject(sw, "name", "TigerOS LED");
    cJSON_AddStringToObject(sw, "unique_id", (device_id + "_led").c_str());
    cJSON_AddStringToObject(sw, "object_id", (device_id + "_led").c_str());
    cJSON_AddStringToObject(sw, "command_topic", command_topic.c_str());
    cJSON_AddStringToObject(sw, "payload_on", "{\"command\":\"control\",\"target\":\"led\",\"value\":true}");
    cJSON_AddStringToObject(sw, "payload_off", "{\"command\":\"control\",\"target\":\"led\",\"value\":false}");
    cJSON_AddBoolToObject(sw, "optimistic", true);
    add_availability(sw);
    add_device(sw);
    if (publish_ha_entity("switch", "led", sw) != ESP_OK) err = ESP_FAIL;
    cJSON_Delete(sw);

    tiger_log(err == ESP_OK ? "INFO" : "WARN", TAG, err == ESP_OK ? "Home Assistant discovery published" : "Home Assistant discovery publish incomplete");
    return err;
}

void MqttManager::task_entry(void* arg) {
    static_cast<MqttManager*>(arg)->loop();
}

void MqttManager::loop() {
    while (true) {
        if (!config_.enabled) {
            stop_client();
            set_state(MqttConnectionState::Disabled);
        } else if (!wifi_manager().status().connected) {
            stop_client();
            set_state(MqttConnectionState::Disconnected);
        } else if (!client_) {
            start_client();
        } else if (state_ == MqttConnectionState::Connected) {
            publish_periodic();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void MqttManager::start_client() {
    if (!config_.enabled || config_.host.empty() || client_) {
        return;
    }

    uri_ = build_uri();
    will_topic_ = topic("status");
    will_message_ = "{\"online\":false}";

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = uri_.c_str();
    mqtt_cfg.credentials.client_id = config_.client_id.c_str();
    if (!config_.username.empty()) {
        mqtt_cfg.credentials.username = config_.username.c_str();
    }
    if (!config_.password.empty()) {
        mqtt_cfg.credentials.authentication.password = config_.password.c_str();
    }
    mqtt_cfg.session.last_will.topic = will_topic_.c_str();
    mqtt_cfg.session.last_will.msg = will_message_.c_str();
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = true;

    // TLS certificate verification is intentionally left as a placeholder.
    // Production builds should provide a broker CA certificate or bundle.
    client_ = esp_mqtt_client_init(&mqtt_cfg);
    if (!client_) {
        set_state(MqttConnectionState::Error);
        tiger_log("ERROR", TAG, "MQTT client init failed");
        return;
    }
    esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, mqtt_event_handler, this);
    set_state(MqttConnectionState::Connecting);
    tiger_log("INFO", TAG, "Starting MQTT client");
    if (esp_mqtt_client_start(client_) != ESP_OK) {
        tiger_log("ERROR", TAG, "MQTT client start failed");
        stop_client();
        set_state(MqttConnectionState::Error);
    }
}

void MqttManager::stop_client() {
    if (!client_) {
        return;
    }
    if (state_ == MqttConnectionState::Connected) {
        publish_status(false);
    }
    esp_mqtt_client_stop(client_);
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
}

void MqttManager::mqtt_event_handler(void* handler_args, esp_event_base_t, int32_t, void* event_data) {
    static_cast<MqttManager*>(handler_args)->handle_event(static_cast<esp_mqtt_event_handle_t>(event_data));
}

void MqttManager::handle_event(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            set_state(MqttConnectionState::Connected);
            tiger_log("INFO", TAG, "MQTT connected");
            publish_status(true);
            publish_home_assistant_discovery();
            if (esp_mqtt_client_subscribe(client_, topic("command").c_str(), 1) < 0) {
                tiger_log("WARN", TAG, "MQTT command subscribe failed");
            }
            if (esp_mqtt_client_subscribe(client_, topic("ota").c_str(), 1) < 0) {
                tiger_log("WARN", TAG, "MQTT OTA subscribe failed");
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            set_state(MqttConnectionState::Disconnected);
            tiger_log("WARN", TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            tiger_log("INFO", TAG, "MQTT subscribe acknowledged");
            break;
        case MQTT_EVENT_PUBLISHED:
            break;
        case MQTT_EVENT_DATA:
            handle_command(
                std::string(event->topic, event->topic_len),
                std::string(event->data, event->data_len)
            );
            break;
        case MQTT_EVENT_ERROR:
            set_state(MqttConnectionState::Error);
            tiger_log("ERROR", TAG, "MQTT event error");
            break;
        default:
            break;
    }
}

void MqttManager::handle_command(const std::string&, const std::string& payload) {
    tiger_log("INFO", TAG, "MQTT command received");
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        publish_response("unknown", false, "Invalid JSON command");
        return;
    }

    cJSON* command_item = cJSON_GetObjectItem(root, "command");
    const std::string command = cJSON_IsString(command_item) ? command_item->valuestring : "";
    if (command == "reboot") {
        publish_response(command, true, "Reboot scheduled");
        schedule_restart();
    } else if (command == "factory_reset") {
        nvs_store().factory_reset_config();
        publish_response(command, true, "Factory reset scheduled");
        schedule_restart();
    } else if (command == "get_status") {
        publish_status(true);
        publish_response(command, true, "Status published");
    } else if (command == "control") {
        publish_response(command, true, "Control command accepted");
    } else if (command == "ota_check") {
        auto status = device_manager().status();
        auto ota_config = nvs_store().get_ota_config();
        RemoteOtaConfig config{
            ota_config.enabled,
            ota_config.auto_update,
            ota_config.check_url,
            status.firmware_version,
            status.device_id,
            ota_config.channel,
            status.hardware_model,
            status.flash_size,
            status.chip_model
        };
        ota_manager().check_remote_update(config);
        publish_response(command, true, "OTA check invoked");
    } else {
        publish_response(command.empty() ? "unknown" : command, false, "Unsupported command");
    }

    cJSON_Delete(root);
}

void MqttManager::publish_periodic() {
    const uint64_t now = uptime_seconds();
    if (now - last_status_publish_ >= 30) {
        publish_status(true);
        last_status_publish_ = now;
    }
    if (now - last_telemetry_publish_ >= 60) {
        publish_telemetry();
        last_telemetry_publish_ = now;
    }
}

void MqttManager::publish_telemetry() {
    auto status = device_manager().status();
    auto wifi = wifi_manager().status();
    auto ble_readings = ble_sensor_gateway().paired_latest();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "tigeros.gateway_telemetry.v1");
    cJSON_AddStringToObject(root, "gateway_id", status.device_id.c_str());
    cJSON_AddNumberToObject(root, "sent_at_uptime", static_cast<double>(status.uptime_seconds));
    cJSON_AddNumberToObject(root, "uptime", static_cast<double>(status.uptime_seconds));
    cJSON_AddNumberToObject(root, "free_heap", status.free_heap);
    cJSON_AddNumberToObject(root, "rssi", wifi.rssi);
    cJSON_AddNumberToObject(root, "ble_sensor_count", static_cast<double>(ble_readings.size()));
    cJSON_AddNumberToObject(root, "paired_ble_sensor_count", static_cast<double>(ble_readings.size()));
    cJSON_AddNumberToObject(root, "discovered_ble_sensor_count", static_cast<double>(ble_sensor_gateway().discovered_count()));
    cJSON* sensors = cJSON_AddArrayToObject(root, "ble_sensors");
    for (const auto& reading : ble_readings) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", reading.mac_address.c_str());
        cJSON_AddStringToObject(item, "sensor_mac", reading.mac_address.c_str());
        cJSON_AddStringToObject(item, "name", reading.display_name.empty() ? reading.mac_address.c_str() : reading.display_name.c_str());
        cJSON_AddStringToObject(item, "sensor_name", reading.display_name.empty() ? reading.mac_address.c_str() : reading.display_name.c_str());
        cJSON_AddStringToObject(item, "brand", reading.brand.c_str());
        cJSON_AddStringToObject(item, "model", reading.model.c_str());
        cJSON_AddStringToObject(item, "protocol", reading.protocol.c_str());
        cJSON_AddStringToObject(item, "location", reading.location.c_str());
        cJSON_AddStringToObject(item, "parse_status", reading.parse_status.c_str());
        cJSON_AddNumberToObject(item, "last_seen", static_cast<double>(reading.last_seen));
        cJSON_AddNumberToObject(item, "collected_at_uptime", static_cast<double>(reading.last_seen));
        cJSON_AddNumberToObject(item, "sent_at_uptime", static_cast<double>(status.uptime_seconds));
        cJSON_AddNumberToObject(item, "rssi", reading.rssi);
        if (reading.has_temperature) cJSON_AddNumberToObject(item, "temperature_c", reading.temperature_c);
        if (reading.has_humidity) cJSON_AddNumberToObject(item, "humidity_percent", reading.humidity_percent);
        if (reading.has_battery) cJSON_AddNumberToObject(item, "battery_percent", reading.battery_percent);
        if (reading.has_external_probe_temperature) {
            cJSON_AddNumberToObject(item, "external_probe_temperature_c", reading.external_probe_temperature_c);
        }
        cJSON_AddItemToArray(sensors, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    const int msg_id = esp_mqtt_client_publish(client_, topic("telemetry").c_str(), text, 0, 0, 0);
    cJSON_free(text);
    cJSON_Delete(root);
    if (msg_id < 0) {
        tiger_log("WARN", TAG, "MQTT telemetry publish failed");
    } else {
        tiger_log("INFO", TAG, "MQTT telemetry published");
    }
}

void MqttManager::publish_response(const std::string& command, bool ok, const std::string& message) {
    if (!client_ || state_ != MqttConnectionState::Connected) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "command", command.c_str());
    cJSON_AddStringToObject(root, "message", message.c_str());
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(uptime_seconds()));
    char* text = cJSON_PrintUnformatted(root);
    const int msg_id = esp_mqtt_client_publish(client_, topic("response").c_str(), text, 0, 1, 0);
    cJSON_free(text);
    cJSON_Delete(root);
    tiger_log(msg_id < 0 ? "WARN" : "INFO", TAG, msg_id < 0 ? "MQTT command response failed" : "MQTT command response sent");
}

esp_err_t MqttManager::publish_ha_entity(const char* component, const char* object_suffix, cJSON* payload) {
    char* text = cJSON_PrintUnformatted(payload);
    const int msg_id = esp_mqtt_client_publish(client_, ha_config_topic(component, object_suffix).c_str(), text, 0, 1, 1);
    cJSON_free(text);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

std::string MqttManager::topic(const char* suffix) {
    return "tigeros/" + nvs_store().get_or_create_device_id() + "/" + suffix;
}

std::string MqttManager::ha_config_topic(const char* component, const char* object_suffix) {
    return config_.ha_discovery_prefix + "/" + component + "/" + nvs_store().get_or_create_device_id() + "/" + object_suffix + "/config";
}

std::string MqttManager::build_uri() {
    const char* scheme = config_.use_tls ? "mqtts" : "mqtt";
    return std::string(scheme) + "://" + config_.host + ":" + std::to_string(config_.port);
}

void MqttManager::set_state(MqttConnectionState state) {
    state_ = state;
}

MqttManager& mqtt_manager() {
    static MqttManager manager;
    return manager;
}

} // namespace tigeros
