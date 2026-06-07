#include "auth_manager.h"

#include <array>
#include <cstring>

#include "esp_check.h"
#include "mbedtls/sha256.h"
#include "nvs_store.h"
#include "tiger_log.h"

namespace tigeros {
namespace {

constexpr const char* DEFAULT_USERNAME = "admin";
constexpr const char* DEFAULT_PASSWORD = "tigeros";
constexpr const char* PASSWORD_SALT = "TigerOS:v0.2:local-console";

std::string default_password_hash() {
    return tiger_sha256_hex(std::string(PASSWORD_SALT) + ":" + DEFAULT_PASSWORD);
}

bool constant_time_equal(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

} // namespace

esp_err_t AuthManager::init() {
    password_hash_ = nvs_store().get_password_hash();
    if (password_hash_.empty()) {
        password_hash_ = default_password_hash();
        ESP_RETURN_ON_ERROR(nvs_store().save_password_hash(password_hash_), "auth", "save default password hash");
        tiger_log("WARN", "auth", "Default admin password is active");
    }
    token_ = nvs_store().get_or_create_api_token();
    return ESP_OK;
}

bool AuthManager::verify_login(const std::string& username, const std::string& password) {
    if (username != DEFAULT_USERNAME) {
        tiger_log("WARN", "auth", "Login failed: bad username");
        return false;
    }
    const std::string hash = tiger_sha256_hex(std::string(PASSWORD_SALT) + ":" + password);
    const bool ok = constant_time_equal(hash, password_hash_);
    tiger_log(ok ? "INFO" : "WARN", "auth", ok ? "Login succeeded" : "Login failed: bad password");
    return ok;
}

bool AuthManager::is_authorized(httpd_req_t* req) {
    char header[96] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return false;
    }

    constexpr const char* prefix = "Bearer ";
    if (std::strncmp(header, prefix, std::strlen(prefix)) != 0) {
        return false;
    }
    return constant_time_equal(token_, header + std::strlen(prefix));
}

std::string AuthManager::api_token() {
    return token_;
}

std::string tiger_sha256_hex(const std::string& input) {
    std::array<unsigned char, 32> digest{};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data(), 0);

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    return out;
}

AuthManager& auth_manager() {
    static AuthManager manager;
    return manager;
}

} // namespace tigeros
