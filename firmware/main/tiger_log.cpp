#include "tiger_log.h"

#include <array>
#include <cstdarg>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace tigeros {
namespace {

constexpr size_t LOG_CAPACITY = 256;

std::array<TigerLogEntry, LOG_CAPACITY> entries;
size_t write_index = 0;
size_t entry_count = 0;
SemaphoreHandle_t mutex = nullptr;

SemaphoreHandle_t log_mutex() {
    if (!mutex) {
        mutex = xSemaphoreCreateMutex();
    }
    return mutex;
}

} // namespace

void tiger_log(const char* level, const char* tag, const char* message) {
    ESP_LOGI(tag, "[%s] %s", level, message);

    TigerLogEntry entry;
    entry.uptime_seconds = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    entry.level = level ? level : "INFO";
    entry.tag = tag ? tag : "app";
    entry.message = message ? message : "";

    SemaphoreHandle_t lock = log_mutex();
    if (lock) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }
    entries[write_index] = entry;
    write_index = (write_index + 1) % LOG_CAPACITY;
    if (entry_count < LOG_CAPACITY) {
        ++entry_count;
    }
    if (lock) {
        xSemaphoreGive(lock);
    }
}

std::vector<TigerLogEntry> tiger_logs() {
    std::vector<TigerLogEntry> out;
    SemaphoreHandle_t lock = log_mutex();
    if (lock) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }
    out.reserve(entry_count);
    const size_t start = entry_count == LOG_CAPACITY ? write_index : 0;
    for (size_t i = 0; i < entry_count; ++i) {
        out.push_back(entries[(start + i) % LOG_CAPACITY]);
    }
    if (lock) {
        xSemaphoreGive(lock);
    }
    return out;
}

void tiger_logs_clear() {
    SemaphoreHandle_t lock = log_mutex();
    if (lock) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }
    write_index = 0;
    entry_count = 0;
    if (lock) {
        xSemaphoreGive(lock);
    }
    ESP_LOGI("tiger_log", "Log ring buffer cleared");
}

} // namespace tigeros
