#pragma once

#include <string>

#include "esp_err.h"
#include "mqtt_client.h"
#include "nvs_store.h"

struct cJSON;

namespace tigeros {

enum class MqttConnectionState {
    Disabled,
    Disconnected,
    Connecting,
    Connected,
    Error,
};

struct MqttStatus {
    bool enabled = false;
    MqttConnectionState state = MqttConnectionState::Disabled;
    std::string state_text;
    std::string host;
    int port = 1883;
    std::string client_id;
    bool use_tls = false;
};

class MqttManager {
public:
    esp_err_t init();
    esp_err_t apply_config(const MqttConfig& config);
    MqttConfig config();
    MqttStatus status();
    esp_err_t publish_status(bool online);
    esp_err_t publish_log(const std::string& level, const std::string& tag, const std::string& message);
    esp_err_t publish_raw(const std::string& topic, const std::string& payload, int qos = 0, bool retain = false);
    esp_err_t publish_home_assistant_discovery();

private:
    static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    static void task_entry(void* arg);
    void loop();
    void start_client();
    void stop_client();
    void handle_event(esp_mqtt_event_handle_t event);
    void handle_command(const std::string& topic, const std::string& payload);
    void publish_periodic();
    void publish_telemetry();
    void publish_response(const std::string& command, bool ok, const std::string& message);
    esp_err_t publish_ha_entity(const char* component, const char* object_suffix, struct cJSON* payload);
    std::string topic(const char* suffix);
    std::string ha_config_topic(const char* component, const char* object_suffix);
    std::string build_uri();
    void set_state(MqttConnectionState state);

    bool initialized_ = false;
    bool task_started_ = false;
    esp_mqtt_client_handle_t client_ = nullptr;
    MqttConfig config_;
    MqttConnectionState state_ = MqttConnectionState::Disabled;
    std::string uri_;
    std::string will_topic_;
    std::string will_message_;
    uint64_t last_status_publish_ = 0;
    uint64_t last_telemetry_publish_ = 0;
};

MqttManager& mqtt_manager();

} // namespace tigeros
