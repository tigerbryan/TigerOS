#pragma once

#include <string>

#include "cJSON.h"
#include "esp_err.h"

namespace tigeros {

struct BoardSupportStatus {
    std::string profile;
    std::string display_driver = "ST7796/ST7365";
    int display_width = 320;
    int display_height = 480;
    int backlight_gpio = 2;
    bool display_available = true;
    bool audio_available = true;
    bool microphone_available = true;
    bool camera_available = true;
    bool display_enabled = true;
    bool display_backlight_on = true;
    bool audio_enabled = false;
    bool microphone_enabled = false;
    bool camera_enabled = false;
    std::string display_mode = "status";
    std::string audio_mode = "placeholder";
    std::string note;
};

class BoardSupport {
public:
    esp_err_t init();
    BoardSupportStatus status() const;
    cJSON* to_json() const;
    esp_err_t set_display_enabled(bool enabled);
    esp_err_t set_display_backlight(bool enabled);
    esp_err_t play_beep(const std::string& kind);

private:
    bool initialized_ = false;
    bool display_enabled_ = true;
    bool display_backlight_on_ = true;
};

BoardSupport& board_support();

} // namespace tigeros
