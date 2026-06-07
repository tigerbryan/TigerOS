#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

namespace tigeros {

esp_err_t register_api_routes(httpd_handle_t server);

} // namespace tigeros
