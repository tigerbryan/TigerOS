#include "cloud_client.h"
#include "device_manager.h"
#include "device_registry.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "auth_manager.h"
#include "board_support.h"
#include "ble_manager.h"
#include "ble_sensor_gateway.h"
#include "mqtt_manager.h"
#include "nvs_store.h"
#include "ota_manager.h"
#include "tiger_log.h"
#include "tft_display.h"
#include "web_server.h"
#include "wifi_manager.h"

namespace {

constexpr const char* TAG = "TigerOS";
constexpr const char* TIGEROS_FW_VERSION = "1.0.70-wifi-connect-diagnostics";
constexpr const char* TIGEROS_BUILD_TIME = __DATE__ " " __TIME__;

} // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting TigerOS firmware v%s", TIGEROS_FW_VERSION);

    ESP_ERROR_CHECK(tigeros::nvs_store().init());
    tigeros::device_manager().init(TIGEROS_FW_VERSION, TIGEROS_BUILD_TIME);
    ESP_ERROR_CHECK(tigeros::board_support().init());
    ESP_ERROR_CHECK(tigeros::auth_manager().init());
    ESP_ERROR_CHECK(tigeros::wifi_manager().init());
    ESP_ERROR_CHECK(tigeros::wifi_manager().start());
    ESP_ERROR_CHECK(tigeros::web_server().start());
    ESP_ERROR_CHECK(tigeros::ble_manager().init());
    ESP_ERROR_CHECK(tigeros::device_registry().init());
    ESP_ERROR_CHECK(tigeros::ble_sensor_gateway().init());
    ESP_ERROR_CHECK(tigeros::tft_display().init());
    ESP_ERROR_CHECK(tigeros::cloud_client().init());
    ESP_ERROR_CHECK(tigeros::ota_manager().init());
    esp_err_t ota_valid_err = tigeros::ota_manager().mark_app_valid_after_successful_boot();
    if (ota_valid_err != ESP_OK) {
        ESP_LOGW(TAG, "OTA rollback validation skipped: %s", esp_err_to_name(ota_valid_err));
    }

    ESP_LOGI(TAG, "TigerOS cloud heartbeat build is ready");
    tigeros::tiger_log("INFO", TAG, "TigerOS boot complete; HTTP webhook and cloud heartbeat are available");
}
