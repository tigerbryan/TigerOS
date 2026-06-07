#include "web_server.h"

#include <cstring>

#include "api_routes.h"
#include "esp_log.h"

namespace tigeros {
namespace {

constexpr const char* TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");
extern const uint8_t i18n_en_js_start[] asm("_binary_en_js_start");
extern const uint8_t i18n_en_js_end[] asm("_binary_en_js_end");
extern const uint8_t i18n_zh_cn_js_start[] asm("_binary_zh_CN_js_start");
extern const uint8_t i18n_zh_cn_js_end[] asm("_binary_zh_CN_js_end");

esp_err_t send_embedded(httpd_req_t* req, const uint8_t* start, const uint8_t* end, const char* type) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char*>(start), end - start);
}

esp_err_t index_handler(httpd_req_t* req) {
    return send_embedded(req, index_html_start, index_html_end, "text/html; charset=utf-8");
}

esp_err_t app_js_handler(httpd_req_t* req) {
    return send_embedded(req, app_js_start, app_js_end, "application/javascript");
}

esp_err_t style_css_handler(httpd_req_t* req) {
    return send_embedded(req, style_css_start, style_css_end, "text/css");
}

esp_err_t i18n_en_handler(httpd_req_t* req) {
    return send_embedded(req, i18n_en_js_start, i18n_en_js_end, "application/javascript");
}

esp_err_t i18n_zh_cn_handler(httpd_req_t* req) {
    return send_embedded(req, i18n_zh_cn_js_start, i18n_zh_cn_js_end, "application/javascript");
}

httpd_uri_t uri(const char* path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t out = {};
    out.uri = path;
    out.method = method;
    out.handler = handler;
    out.user_ctx = nullptr;
    return out;
}

} // namespace

esp_err_t WebServer::start() {
    if (server_) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 45;
    config.send_wait_timeout = 45;
    config.max_uri_handlers = 60;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t index = uri("/", HTTP_GET, index_handler);
    httpd_uri_t index_wildcard = uri("/index.html", HTTP_GET, index_handler);
    httpd_uri_t app_js = uri("/app.js", HTTP_GET, app_js_handler);
    httpd_uri_t style_css = uri("/style.css", HTTP_GET, style_css_handler);
    httpd_uri_t i18n_en = uri("/i18n/en.js", HTTP_GET, i18n_en_handler);
    httpd_uri_t i18n_zh_cn = uri("/i18n/zh-CN.js", HTTP_GET, i18n_zh_cn_handler);

    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &index_wildcard));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &app_js));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &style_css));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &i18n_en));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &i18n_zh_cn));
    ESP_ERROR_CHECK(register_api_routes(server_));

    ESP_LOGI(TAG, "Web console started on port 80");
    return ESP_OK;
}

esp_err_t WebServer::stop() {
    if (!server_) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(server_);
    server_ = nullptr;
    return err;
}

WebServer& web_server() {
    static WebServer server;
    return server;
}

} // namespace tigeros
