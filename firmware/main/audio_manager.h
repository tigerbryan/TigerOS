#pragma once

#include <string>

#include "esp_err.h"

namespace tigeros {

class AudioManager {
public:
    esp_err_t init();
    bool available() const { return available_; }
    bool enabled() const { return enabled_; }
    esp_err_t play_beep(const std::string& kind);

private:
    bool initialized_ = false;
    bool available_ = false;
    bool enabled_ = false;
};

AudioManager& audio_manager();

} // namespace tigeros
