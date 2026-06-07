#include "tft_display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "device_manager.h"
#include "device_registry.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tiger_log.h"
#include "wifi_manager.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "tft_display";
constexpr int LCD_WIDTH = 320;
constexpr int LCD_HEIGHT = 480;
constexpr spi_host_device_t LCD_SPI_HOST = SPI2_HOST;
constexpr gpio_num_t LCD_MOSI = GPIO_NUM_21;
constexpr gpio_num_t LCD_SCLK = GPIO_NUM_47;
constexpr gpio_num_t LCD_DC = GPIO_NUM_45;
constexpr gpio_num_t LCD_RST = GPIO_NUM_20;
constexpr gpio_num_t LCD_BL = GPIO_NUM_2;
constexpr int LCD_PIXEL_CLOCK_HZ = 10 * 1000 * 1000;
constexpr int LCD_LINE_PIXELS = 160;

spi_device_handle_t lcd_spi = nullptr;
uint16_t line_buffer[LCD_LINE_PIXELS];

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return static_cast<uint16_t>((value >> 8) | (value << 8));
}

constexpr uint16_t C_BG = 0x0000;
const uint16_t C_PANEL = rgb565(22, 31, 39);
const uint16_t C_PANEL_2 = rgb565(30, 43, 53);
const uint16_t C_TEXT = rgb565(245, 242, 234);
const uint16_t C_MUTED = rgb565(158, 170, 184);
const uint16_t C_ACCENT = rgb565(246, 166, 48);
const uint16_t C_GOOD = rgb565(104, 211, 145);
const uint16_t C_BAD = rgb565(222, 74, 78);
const uint16_t C_LINE = rgb565(53, 72, 88);

void tx_cmd(uint8_t cmd, const void* data = nullptr, size_t len = 0) {
    if (!lcd_spi) {
        return;
    }
    gpio_set_level(LCD_DC, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = cmd;
    spi_device_polling_transmit(lcd_spi, &t);

    if (data && len > 0) {
        gpio_set_level(LCD_DC, 1);
        spi_transaction_t d = {};
        d.length = len * 8;
        d.tx_buffer = data;
        spi_device_polling_transmit(lcd_spi, &d);
    }
}

void tx_pixels(const void* data, size_t len) {
    if (!lcd_spi || !data || len == 0) {
        return;
    }
    gpio_set_level(LCD_DC, 1);
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(lcd_spi, &t);
}

void set_window(int x0, int y0, int x1, int y1) {
    uint8_t data[4] = {
        static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0 & 0xFF),
        static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1 & 0xFF),
    };
    tx_cmd(0x2A, data, sizeof(data));
    data[0] = static_cast<uint8_t>(y0 >> 8);
    data[1] = static_cast<uint8_t>(y0 & 0xFF);
    data[2] = static_cast<uint8_t>(y1 >> 8);
    data[3] = static_cast<uint8_t>(y1 & 0xFF);
    tx_cmd(0x2B, data, sizeof(data));
}

void fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!lcd_spi || w <= 0 || h <= 0) return;
    x = std::max(0, x);
    y = std::max(0, y);
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    w = std::min(w, LCD_WIDTH - x);
    h = std::min(h, LCD_HEIGHT - y);

    set_window(x, y, x + w - 1, y + h - 1);
    tx_cmd(0x2C);
    const int chunk = std::min<int>(w, LCD_LINE_PIXELS);
    for (int i = 0; i < chunk; ++i) line_buffer[i] = color;
    for (int row = 0; row < h; ++row) {
        int remaining = w;
        while (remaining > 0) {
            int n = std::min(remaining, chunk);
            tx_pixels(line_buffer, n * sizeof(uint16_t));
            remaining -= n;
        }
    }
}

const uint8_t* glyph(char ch) {
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t unknown[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
    switch (ch) {
        case ' ': return blank;
        case '-': { static const uint8_t g[5] = {0x08,0x08,0x08,0x08,0x08}; return g; }
        case '.': { static const uint8_t g[5] = {0,0,0,0x60,0x60}; return g; }
        case ':': { static const uint8_t g[5] = {0,0x36,0x36,0,0}; return g; }
        case '/': { static const uint8_t g[5] = {0x40,0x30,0x08,0x06,0x01}; return g; }
        case '%': { static const uint8_t g[5] = {0x63,0x13,0x08,0x64,0x63}; return g; }
        case '0': { static const uint8_t g[5] = {0x3E,0x51,0x49,0x45,0x3E}; return g; }
        case '1': { static const uint8_t g[5] = {0,0x42,0x7F,0x40,0}; return g; }
        case '2': { static const uint8_t g[5] = {0x42,0x61,0x51,0x49,0x46}; return g; }
        case '3': { static const uint8_t g[5] = {0x21,0x41,0x45,0x4B,0x31}; return g; }
        case '4': { static const uint8_t g[5] = {0x18,0x14,0x12,0x7F,0x10}; return g; }
        case '5': { static const uint8_t g[5] = {0x27,0x45,0x45,0x45,0x39}; return g; }
        case '6': { static const uint8_t g[5] = {0x3C,0x4A,0x49,0x49,0x30}; return g; }
        case '7': { static const uint8_t g[5] = {0x01,0x71,0x09,0x05,0x03}; return g; }
        case '8': { static const uint8_t g[5] = {0x36,0x49,0x49,0x49,0x36}; return g; }
        case '9': { static const uint8_t g[5] = {0x06,0x49,0x49,0x29,0x1E}; return g; }
        case 'A': { static const uint8_t g[5] = {0x7E,0x11,0x11,0x11,0x7E}; return g; }
        case 'B': { static const uint8_t g[5] = {0x7F,0x49,0x49,0x49,0x36}; return g; }
        case 'C': { static const uint8_t g[5] = {0x3E,0x41,0x41,0x41,0x22}; return g; }
        case 'D': { static const uint8_t g[5] = {0x7F,0x41,0x41,0x22,0x1C}; return g; }
        case 'E': { static const uint8_t g[5] = {0x7F,0x49,0x49,0x49,0x41}; return g; }
        case 'F': { static const uint8_t g[5] = {0x7F,0x09,0x09,0x09,0x01}; return g; }
        case 'G': { static const uint8_t g[5] = {0x3E,0x41,0x49,0x49,0x7A}; return g; }
        case 'H': { static const uint8_t g[5] = {0x7F,0x08,0x08,0x08,0x7F}; return g; }
        case 'I': { static const uint8_t g[5] = {0,0x41,0x7F,0x41,0}; return g; }
        case 'J': { static const uint8_t g[5] = {0x20,0x40,0x41,0x3F,0x01}; return g; }
        case 'K': { static const uint8_t g[5] = {0x7F,0x08,0x14,0x22,0x41}; return g; }
        case 'L': { static const uint8_t g[5] = {0x7F,0x40,0x40,0x40,0x40}; return g; }
        case 'M': { static const uint8_t g[5] = {0x7F,0x02,0x0C,0x02,0x7F}; return g; }
        case 'N': { static const uint8_t g[5] = {0x7F,0x04,0x08,0x10,0x7F}; return g; }
        case 'O': { static const uint8_t g[5] = {0x3E,0x41,0x41,0x41,0x3E}; return g; }
        case 'P': { static const uint8_t g[5] = {0x7F,0x09,0x09,0x09,0x06}; return g; }
        case 'Q': { static const uint8_t g[5] = {0x3E,0x41,0x51,0x21,0x5E}; return g; }
        case 'R': { static const uint8_t g[5] = {0x7F,0x09,0x19,0x29,0x46}; return g; }
        case 'S': { static const uint8_t g[5] = {0x46,0x49,0x49,0x49,0x31}; return g; }
        case 'T': { static const uint8_t g[5] = {0x01,0x01,0x7F,0x01,0x01}; return g; }
        case 'U': { static const uint8_t g[5] = {0x3F,0x40,0x40,0x40,0x3F}; return g; }
        case 'V': { static const uint8_t g[5] = {0x1F,0x20,0x40,0x20,0x1F}; return g; }
        case 'W': { static const uint8_t g[5] = {0x3F,0x40,0x38,0x40,0x3F}; return g; }
        case 'X': { static const uint8_t g[5] = {0x63,0x14,0x08,0x14,0x63}; return g; }
        case 'Y': { static const uint8_t g[5] = {0x07,0x08,0x70,0x08,0x07}; return g; }
        case 'Z': { static const uint8_t g[5] = {0x61,0x51,0x49,0x45,0x43}; return g; }
        default: return unknown;
    }
}

void draw_char(int x, int y, char ch, uint16_t color, int scale) {
    const uint8_t* g = glyph(ch);
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if (g[col] & (1 << row)) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

void draw_text(int x, int y, const char* text, uint16_t color, int scale = 2, int max_width = LCD_WIDTH) {
    int cx = x;
    const int advance = 6 * scale;
    for (const char* p = text; p && *p; ++p) {
        unsigned char ch = static_cast<unsigned char>(*p);
        if (ch > 0x7E) {
            ch = '?';
        }
        if (cx + 5 * scale > max_width) break;
        draw_char(cx, y, static_cast<char>(ch), color, scale);
        cx += advance;
    }
}

std::string state_summary(const UniversalDevice& device) {
    cJSON* root = cJSON_Parse(device.state_json.c_str());
    if (!root) return "NO DATA";
    char out[96] = {};
    cJSON* probe = cJSON_GetObjectItem(root, "external_probe_temperature_c");
    cJSON* temp = cJSON_GetObjectItem(root, "temperature_c");
    cJSON* humidity = cJSON_GetObjectItem(root, "humidity_percent");
    cJSON* battery = cJSON_GetObjectItem(root, "battery_percent");
    if (cJSON_IsNumber(probe)) {
        std::snprintf(out, sizeof(out), "EXT %.1fC", probe->valuedouble);
    } else if (cJSON_IsNumber(temp)) {
        std::snprintf(out, sizeof(out), "%.1fC", temp->valuedouble);
    }
    if (cJSON_IsNumber(humidity)) {
        char part[32];
        std::snprintf(part, sizeof(part), " %.0f%%", humidity->valuedouble);
        std::strncat(out, part, sizeof(out) - std::strlen(out) - 1);
    }
    if (cJSON_IsNumber(battery)) {
        char part[32];
        std::snprintf(part, sizeof(part), " B%d%%", battery->valueint);
        std::strncat(out, part, sizeof(out) - std::strlen(out) - 1);
    }
    cJSON_Delete(root);
    return out[0] ? out : "RAW";
}

std::string ascii_label(const UniversalDevice& device) {
    std::string label = device.name.empty() ? device.id : device.name;
    std::string out;
    for (char ch : label) {
        if (static_cast<unsigned char>(ch) <= 0x7E && ch >= 0x20) out.push_back(ch);
    }
    if (out.empty()) out = device.brand.empty() ? device.id : device.brand;
    if (out.size() > 18) out.resize(18);
    return out;
}

} // namespace

esp_err_t TftDisplay::init() {
    if (initialized_) return ESP_OK;
    initialized_ = true;
    esp_err_t err = init_panel();
    if (err != ESP_OK) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "TFT display init failed: %s", esp_err_to_name(err));
        tiger_log("WARN", TAG, msg);
        return ESP_OK;
    }
    available_ = true;
    enabled_ = true;
    xTaskCreate(task_entry, "tft_display", 5120, this, 2, nullptr);
    tiger_log("INFO", TAG, "TFT status display initialized 320x480 ST7796/ST7365 SPI");
    return ESP_OK;
}

esp_err_t TftDisplay::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (lcd_spi) {
        tx_cmd(enabled ? 0x29 : 0x28);
    }
    return ESP_OK;
}

esp_err_t TftDisplay::init_panel() {
    gpio_config_t rst = {};
    rst.pin_bit_mask = (1ULL << LCD_RST) | (1ULL << LCD_BL) | (1ULL << LCD_DC);
    rst.mode = GPIO_MODE_OUTPUT;
    rst.pull_up_en = GPIO_PULLUP_DISABLE;
    rst.pull_down_en = GPIO_PULLDOWN_DISABLE;
    rst.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "reset gpio");
    gpio_set_level(LCD_BL, 1);

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = LCD_SCLK;
    buscfg.mosi_io_num = LCD_MOSI;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = LCD_LINE_PIXELS * sizeof(uint16_t);
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = LCD_PIXEL_CLOCK_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;
    devcfg.queue_size = 1;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI_HOST, &devcfg, &lcd_spi), TAG, "spi device");

    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    tx_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(120));

    tx_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t data = 0xC3;
    tx_cmd(0xF0, &data, 1);
    data = 0x96;
    tx_cmd(0xF0, &data, 1);

    // Match the official TFT_eSPI ST7796 init table used by Freenove.
    data = 0x48;
    tx_cmd(0x36, &data, 1);
    data = 0x55;
    tx_cmd(0x3A, &data, 1);
    data = 0x01;
    tx_cmd(0xB4, &data, 1);
    const uint8_t b6[] = {0x80, 0x02, 0x3B};
    tx_cmd(0xB6, b6, sizeof(b6));
    const uint8_t e8[] = {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33};
    tx_cmd(0xE8, e8, sizeof(e8));
    data = 0x06;
    tx_cmd(0xC1, &data, 1);
    data = 0xA7;
    tx_cmd(0xC2, &data, 1);
    data = 0x18;
    tx_cmd(0xC5, &data, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t e0[] = {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B};
    tx_cmd(0xE0, e0, sizeof(e0));
    const uint8_t e1[] = {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B};
    tx_cmd(0xE1, e1, sizeof(e1));
    vTaskDelay(pdMS_TO_TICKS(120));
    data = 0x3C;
    tx_cmd(0xF0, &data, 1);
    data = 0x69;
    tx_cmd(0xF0, &data, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    tx_cmd(0x21);
    tx_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));
    fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    return ESP_OK;
}

void TftDisplay::task_entry(void* arg) {
    static_cast<TftDisplay*>(arg)->task_loop();
}

void TftDisplay::task_loop() {
    constexpr int64_t kFallbackRefreshUs = 60LL * 1000LL * 1000LL;
    while (true) {
        if (enabled_ && available_) {
            const int64_t now_us = esp_timer_get_time();
            std::string signature = build_signature();
            const bool changed = signature != last_signature_;
            const bool stale = last_render_us_ == 0 || now_us - last_render_us_ >= kFallbackRefreshUs;
            if (changed || stale) {
                last_signature_ = std::move(signature);
                last_render_us_ = now_us;
                render();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

std::string TftDisplay::build_signature() {
    auto status = device_manager().status();
    auto wifi = wifi_manager().status();
    auto devices = device_registry().devices();

    char head[160];
    // Heap moves constantly; bucket it so tiny allocator noise does not make the LCD flicker.
    const uint32_t heap_bucket = static_cast<uint32_t>(status.free_heap / 4096);
    std::snprintf(head,
                  sizeof(head),
                  "w:%d|ip:%s|heap:%lu|count:%u",
                  wifi.connected ? 1 : 0,
                  status.ip_address.c_str(),
                  static_cast<unsigned long>(heap_bucket),
                  static_cast<unsigned>(devices.size()));

    std::string signature(head);
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    int included = 0;
    for (const auto& d : devices) {
        if (d.last_seen == 0 || d.state_json.empty()) continue;
        const bool fresh = now >= d.last_seen && now - d.last_seen < 180;
        signature.append("|");
        signature.append(d.id);
        signature.append(":");
        signature.append(d.name);
        signature.append(":");
        signature.append(fresh ? "1:" : "0:");
        signature.append(d.state_json);
        if (++included >= 3) break;
    }
    return signature;
}

void TftDisplay::render() {
    fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    fill_rect(0, 0, LCD_WIDTH, 64, C_PANEL);
    draw_text(12, 12, "TigerOS", C_ACCENT, 3);

    auto status = device_manager().status();
    auto wifi = wifi_manager().status();
    char line[96];
    std::snprintf(line, sizeof(line), "%s", wifi.connected ? "ONLINE" : "SETUP");
    draw_text(218, 18, line, wifi.connected ? C_GOOD : C_BAD, 2);

    fill_rect(12, 82, 296, 84, C_PANEL);
    std::snprintf(line, sizeof(line), "IP %s", status.ip_address.c_str());
    draw_text(24, 98, line, C_TEXT, 2);
    std::snprintf(line, sizeof(line), "HEAP %lu", static_cast<unsigned long>(status.free_heap));
    draw_text(24, 128, line, C_MUTED, 2);

    auto devices = device_registry().devices();
    int readable = 0;
    for (const auto& d : devices) {
        if (d.last_seen > 0 && !d.state_json.empty()) readable++;
    }
    fill_rect(12, 184, 296, 48, C_PANEL_2);
    std::snprintf(line, sizeof(line), "DEVICES %u  DATA %d", static_cast<unsigned>(devices.size()), readable);
    draw_text(24, 200, line, C_TEXT, 2);

    int y = 250;
    int shown = 0;
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    for (const auto& d : devices) {
        if (d.last_seen == 0 || d.state_json.empty()) continue;
        fill_rect(12, y, 296, 58, C_PANEL);
        draw_text(22, y + 10, ascii_label(d).c_str(), C_TEXT, 2);
        std::string summary = state_summary(d);
        draw_text(22, y + 34, summary.c_str(), C_ACCENT, 2);
        const bool fresh = now >= d.last_seen && now - d.last_seen < 180;
        draw_text(250, y + 10, fresh ? "ON" : "OLD", fresh ? C_GOOD : C_BAD, 2);
        y += 68;
        if (++shown >= 3) break;
    }
    if (shown == 0) {
        draw_text(34, 278, "NO SENSOR DATA", C_MUTED, 2);
    }

    fill_rect(0, LCD_HEIGHT - 28, LCD_WIDTH, 28, C_PANEL);
    std::snprintf(line, sizeof(line), "WEB %s", status.ip_address.c_str());
    draw_text(12, LCD_HEIGHT - 20, line, C_MUTED, 1);
}

TftDisplay& tft_display() {
    static TftDisplay instance;
    return instance;
}

} // namespace tigeros
