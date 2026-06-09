#pragma once

#include "esp_err.h"
#include "nvs_store.h"

#include <string>

struct cJSON;

namespace tigeros {

struct WebhookSendResult {
    esp_err_t err = ESP_OK;
    int sent_count = 0;
    int skipped_count = 0;
    int failed_count = 0;
};

class CloudClient {
public:
    esp_err_t init();
    esp_err_t loop_once();
    WebhookConfig config() const;
    esp_err_t save_webhook_config(const WebhookConfig& config);
    esp_err_t send_webhook_now(WebhookSendResult* result = nullptr);
    esp_err_t queue_webhook_send();

private:
    bool initialized_ = false;
    bool task_started_ = false;
    volatile bool webhook_send_in_progress_ = false;
    uint64_t last_webhook_send_ = 0;
    uint64_t last_heartbeat_send_ = 0;
    bool has_pending_command_result_ = false;
    bool pending_command_ok_ = true;
    std::string pending_command_id_;
    std::string pending_command_name_;
    std::string pending_command_message_;

    static void task_entry(void* arg);
    static void webhook_send_task_entry(void* arg);
    void task_loop();
    WebhookSendResult send_device_webhook(const WebhookConfig& config);
    esp_err_t send_heartbeat(const WebhookConfig& config);
    void handle_cloud_commands(cJSON* commands);
    void record_command_result(const char* id, const char* command, bool ok, const char* message);
};

CloudClient& cloud_client();

} // namespace tigeros
