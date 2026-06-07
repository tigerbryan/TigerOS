#include "board_support.h"

#include <cstdio>

#include "audio_manager.h"
#include "driver/gpio.h"
#include "tiger_log.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "board_support";
constexpr gpio_num_t TFT_RESET_GPIO = GPIO_NUM_20;
constexpr gpio_num_t TFT_BACKLIGHT_GPIO = GPIO_NUM_2;

} // namespace

esp_err_t BoardSupport::init() {
    if (initialized_) {
        return ESP_OK;
    }
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << TFT_RESET_GPIO) | (1ULL << TFT_BACKLIGHT_GPIO);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        tiger_log("ERROR", TAG, esp_err_to_name(err));
        return err;
    }

    gpio_set_level(TFT_RESET_GPIO, 1);
    gpio_set_level(TFT_BACKLIGHT_GPIO, 1);
    display_enabled_ = true;
    display_backlight_on_ = true;
    ESP_ERROR_CHECK_WITHOUT_ABORT(audio_manager().init());
    initialized_ = true;
    tiger_log("INFO", TAG, "Freenove ESP32-S3 media profile loaded; TFT backlight GPIO2 and reset GPIO20 are active");
    return ESP_OK;
}

BoardSupportStatus BoardSupport::status() const {
    BoardSupportStatus out;
    out.profile = "freenove_esp32s3_media_kit";
    out.display_enabled = display_enabled_;
    out.display_backlight_on = display_backlight_on_;
    out.audio_enabled = audio_manager().enabled();
    out.microphone_enabled = false;
    out.camera_enabled = false;
    out.audio_mode = audio_manager().available() ? "i2s_tone" : "unavailable";
    out.note = "3.5 inch Freenove Media Kit profile. LCD uses SPI GPIO21/47/45, reset GPIO20, and backlight GPIO2.";
    return out;
}

cJSON* BoardSupport::to_json() const {
    const BoardSupportStatus s = status();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "profile", s.profile.c_str());
    cJSON_AddStringToObject(root, "display_driver", s.display_driver.c_str());
    cJSON_AddNumberToObject(root, "display_width", s.display_width);
    cJSON_AddNumberToObject(root, "display_height", s.display_height);
    cJSON_AddNumberToObject(root, "backlight_gpio", s.backlight_gpio);
    cJSON_AddBoolToObject(root, "display_available", s.display_available);
    cJSON_AddBoolToObject(root, "audio_available", s.audio_available);
    cJSON_AddBoolToObject(root, "microphone_available", s.microphone_available);
    cJSON_AddBoolToObject(root, "camera_available", s.camera_available);
    cJSON_AddBoolToObject(root, "display_enabled", s.display_enabled);
    cJSON_AddBoolToObject(root, "display_backlight_on", s.display_backlight_on);
    cJSON_AddBoolToObject(root, "audio_enabled", s.audio_enabled);
    cJSON_AddBoolToObject(root, "microphone_enabled", s.microphone_enabled);
    cJSON_AddBoolToObject(root, "camera_enabled", s.camera_enabled);
    cJSON_AddStringToObject(root, "display_mode", s.display_mode.c_str());
    cJSON_AddStringToObject(root, "audio_mode", s.audio_mode.c_str());
    cJSON_AddStringToObject(root, "note", s.note.c_str());
    return root;
}

esp_err_t BoardSupport::set_display_enabled(bool enabled) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    display_enabled_ = enabled;
    esp_err_t err = set_display_backlight(enabled);
    if (err == ESP_OK) {
        tiger_log("INFO", TAG, enabled ? "Display status surface enabled" : "Display status surface disabled");
    }
    return err;
}

esp_err_t BoardSupport::set_display_backlight(bool enabled) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = gpio_set_level(TFT_RESET_GPIO, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = gpio_set_level(TFT_BACKLIGHT_GPIO, enabled ? 1 : 0);
    }
    if (err != ESP_OK) {
        tiger_log("ERROR", TAG, esp_err_to_name(err));
        return err;
    }
    display_backlight_on_ = enabled;
    tiger_log("INFO", TAG, enabled ? "TFT reset released" : "TFT reset asserted");
    return ESP_OK;
}

esp_err_t BoardSupport::play_beep(const std::string& kind) {
    esp_err_t err = audio_manager().play_beep(kind);
    if (err == ESP_OK) {
        char message[96];
        std::snprintf(message, sizeof(message), "Audio beep played kind=%s", kind.empty() ? "default" : kind.c_str());
        tiger_log("INFO", TAG, message);
    }
    return err;
}

BoardSupport& board_support() {
    static BoardSupport instance;
    return instance;
}

} // namespace tigeros
