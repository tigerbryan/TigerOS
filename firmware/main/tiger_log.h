#pragma once

#include <string>
#include <vector>

namespace tigeros {

struct TigerLogEntry {
    uint64_t uptime_seconds = 0;
    std::string level;
    std::string tag;
    std::string message;
};

void tiger_log(const char* level, const char* tag, const char* message);
std::vector<TigerLogEntry> tiger_logs();
void tiger_logs_clear();

} // namespace tigeros
