#pragma once

#include <string>

#include "esp_err.h"

namespace tigeros {

class TftDisplay {
public:
    esp_err_t init();
    bool available() const { return available_; }
    bool enabled() const { return enabled_; }
    esp_err_t set_enabled(bool enabled);

private:
    bool initialized_ = false;
    bool available_ = false;
    bool enabled_ = true;
    std::string last_signature_;

    static void task_entry(void* arg);
    void task_loop();
    esp_err_t init_panel();
    std::string build_signature();
    void render();
};

TftDisplay& tft_display();

} // namespace tigeros
