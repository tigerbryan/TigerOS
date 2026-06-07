#pragma once

#include <string>

#include "esp_err.h"
#include "esp_http_server.h"

namespace tigeros {

class AuthManager {
public:
    esp_err_t init();
    bool verify_login(const std::string& username, const std::string& password);
    bool is_authorized(httpd_req_t* req);
    std::string api_token();

private:
    std::string password_hash_;
    std::string token_;
};

std::string tiger_sha256_hex(const std::string& input);
AuthManager& auth_manager();

} // namespace tigeros
