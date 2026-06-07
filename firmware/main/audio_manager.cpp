#include "audio_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tiger_log.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "audio_manager";
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr gpio_num_t I2S_BCLK = GPIO_NUM_42;
constexpr gpio_num_t I2S_DOUT = GPIO_NUM_1;
constexpr gpio_num_t I2S_LRC = GPIO_NUM_41;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr int16_t DEFAULT_VOLUME = 9000;

i2s_chan_handle_t tx_channel = nullptr;
SemaphoreHandle_t audio_lock = nullptr;

struct BeepSpec {
    uint16_t freq_hz;
    uint16_t duration_ms;
    int16_t volume;
};

BeepSpec spec_for_kind(const std::string& kind) {
    if (kind == "success" || kind == "online") return {880, 90, DEFAULT_VOLUME};
    if (kind == "error" || kind == "alert") return {220, 180, DEFAULT_VOLUME};
    if (kind == "ota") return {660, 120, DEFAULT_VOLUME};
    if (kind == "scan") return {520, 70, DEFAULT_VOLUME};
    return {440, 100, DEFAULT_VOLUME};
}

class ScopedAudioLock {
public:
    explicit ScopedAudioLock(SemaphoreHandle_t lock) : lock_(lock) {
        if (lock_) xSemaphoreTake(lock_, pdMS_TO_TICKS(500));
    }
    ~ScopedAudioLock() {
        if (lock_) xSemaphoreGive(lock_);
    }

private:
    SemaphoreHandle_t lock_;
};

} // namespace

esp_err_t AudioManager::init() {
    if (initialized_) return ESP_OK;
    initialized_ = true;

    audio_lock = xSemaphoreCreateMutex();
    if (!audio_lock) {
        tiger_log("WARN", TAG, "Audio mutex allocation failed");
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_channel, nullptr);
    if (err != ESP_OK) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "I2S channel init failed: %s", esp_err_to_name(err));
        tiger_log("WARN", TAG, msg);
        return ESP_OK;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = I2S_BCLK;
    std_cfg.gpio_cfg.ws = I2S_LRC;
    std_cfg.gpio_cfg.dout = I2S_DOUT;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(tx_channel, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(tx_channel);
    }
    if (err != ESP_OK) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "I2S std mode failed: %s", esp_err_to_name(err));
        tiger_log("WARN", TAG, msg);
        return ESP_OK;
    }

    available_ = true;
    enabled_ = true;
    tiger_log("INFO", TAG, "I2S speaker ready on BCLK=42 LRC=41 DOUT=1");
    return ESP_OK;
}

esp_err_t AudioManager::play_beep(const std::string& kind) {
    if (!available_ || !enabled_ || !tx_channel) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ScopedAudioLock lock(audio_lock);
    const BeepSpec spec = spec_for_kind(kind);
    const uint32_t total_frames = (SAMPLE_RATE * spec.duration_ms) / 1000;
    const uint32_t half_period = std::max<uint32_t>(1, SAMPLE_RATE / (spec.freq_hz * 2));
    int16_t frames[128 * 2];
    uint32_t frame_index = 0;

    while (frame_index < total_frames) {
        const uint32_t chunk = std::min<uint32_t>(128, total_frames - frame_index);
        for (uint32_t i = 0; i < chunk; ++i) {
            const bool high = ((frame_index + i) / half_period) % 2 == 0;
            const int16_t sample = high ? spec.volume : static_cast<int16_t>(-spec.volume);
            frames[i * 2] = sample;
            frames[i * 2 + 1] = sample;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx_channel, frames, chunk * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return err;
        }
        frame_index += chunk;
    }

    std::memset(frames, 0, sizeof(frames));
    size_t written = 0;
    i2s_channel_write(tx_channel, frames, sizeof(frames), &written, pdMS_TO_TICKS(100));
    return ESP_OK;
}

AudioManager& audio_manager() {
    static AudioManager instance;
    return instance;
}

} // namespace tigeros
