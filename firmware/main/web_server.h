#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

namespace tigeros {

class WebServer {
public:
    esp_err_t start();
    esp_err_t stop();

private:
    httpd_handle_t server_ = nullptr;
};

WebServer& web_server();

} // namespace tigeros
