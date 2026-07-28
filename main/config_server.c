#include "config_server.h"
#include "app_config.h"
#include "app_settings.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "config_server";

#define HTTP_RETURN_ON_ERROR(call) do { \
    esp_err_t http_err_ = (call); \
    if (http_err_ != ESP_OK) return http_err_; \
} while (0)

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Deskflow HID 配置</title><style>"
    ":root{color-scheme:light dark;font-family:system-ui,sans-serif}"
    "body{max-width:760px;margin:0 auto;padding:24px;background:#eef2f7;color:#172033}"
    "h1{font-size:1.7rem;margin:0 0 18px}.card{background:#fff;border-radius:14px;"
    "padding:20px;margin:14px 0;box-shadow:0 5px 20px #18315318}"
    "h2{font-size:1.15rem;margin:0 0 14px;color:#2457a7}.grid{display:grid;"
    "grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}"
    "label{display:flex;flex-direction:column;gap:6px;font-size:.9rem;font-weight:600}"
    "input{box-sizing:border-box;width:100%;padding:10px 12px;border:1px solid #b8c3d4;"
    "border-radius:8px;background:#fff;color:#172033;font:inherit}"
    ".wide{grid-column:1/-1}.hid{border-top:1px solid #dce3ed;padding-top:14px;margin-top:14px}"
    ".hid:first-of-type{border-top:0;padding-top:0}.hint{font-size:.84rem;color:#61708a}"
    "button{width:100%;border:0;border-radius:10px;padding:13px;background:#2463d4;"
    "color:#fff;font-size:1rem;font-weight:700;cursor:pointer}"
    "@media(max-width:600px){.grid{grid-template-columns:1fr}.wide{grid-column:auto}}"
    "@media(prefers-color-scheme:dark){body{background:#111827;color:#e5e7eb}.card{background:#1f2937}"
    "input{background:#111827;color:#e5e7eb;border-color:#4b5563}.hint{color:#9ca3af}}"
    "</style></head><body><h1>Deskflow Wi-Fi HID 配置</h1>"
    "<form method=\"post\" action=\"/save\">";

static bool url_decode(char *value)
{
    char *src = value;
    char *dst = value;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            ++src;
        } else if (*src == '%') {
            if (!isxdigit((unsigned char)src[1]) || !isxdigit((unsigned char)src[2]))
                return false;
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = (char)strtoul(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return true;
}

static void html_escape(const char *input, char *output, size_t capacity)
{
    size_t used = 0;
    while (*input && used + 1 < capacity) {
        const char *replacement = NULL;
        switch (*input) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }
        if (replacement) {
            size_t len = strlen(replacement);
            if (used + len >= capacity) break;
            memcpy(output + used, replacement, len);
            used += len;
        } else {
            output[used++] = *input;
        }
        ++input;
    }
    output[used] = '\0';
}

static esp_err_t send_input(httpd_req_t *req, const char *label, const char *name,
                            const char *type, const char *value, const char *extra)
{
    char escaped[400];
    char html[640];
    html_escape(value, escaped, sizeof(escaped));
    int len = snprintf(html, sizeof(html),
        "<label>%s<input name=\"%s\" type=\"%s\" value=\"%s\" %s></label>",
        label, name, type, escaped, extra ? extra : "");
    if (len < 0 || (size_t)len >= sizeof(html)) return ESP_FAIL;
    return httpd_resp_send_chunk(req, html, len);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    const app_settings_t *settings = app_settings_get();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN));
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
        "<section class=\"card\"><h2>通信设置</h2><div class=\"grid\">",
        HTTPD_RESP_USE_STRLEN));
    HTTP_RETURN_ON_ERROR(send_input(req, "Wi-Fi SSID", "wifi_ssid", "text",
                               settings->wifi_ssid, "required maxlength=\"32\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "Wi-Fi 密码", "wifi_password", "password",
                               settings->wifi_password, "maxlength=\"63\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "Deskflow Server IP", "deskflow_host", "text",
                               settings->deskflow_host, "required inputmode=\"decimal\""));
    char port[8];
    snprintf(port, sizeof(port), "%u", settings->deskflow_port);
    HTTP_RETURN_ON_ERROR(send_input(req, "Deskflow Server Port", "deskflow_port", "number",
                               port, "required min=\"1\" max=\"65535\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "SoftAP SSID", "softap_ssid", "text",
                               settings->softap_ssid, "required maxlength=\"32\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "SoftAP 密码（可选）", "softap_password", "password",
                               settings->softap_password, "maxlength=\"63\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "BLE 设备名称", "ble_device_name", "text",
                               settings->ble_device_name,
                               "required maxlength=\"29\" class=\"wide\""));
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
        "</div><p class=\"hint\">密码留空表示开放网络；非空密码长度必须为 8–63 个字符。</p>"
        "</section><section class=\"card\"><h2>HID 设置</h2>",
        HTTPD_RESP_USE_STRLEN));

    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
        char block[128];
        snprintf(block, sizeof(block),
                 "<div class=\"hid\"><strong>HID 设备 %u</strong><div class=\"grid\">",
                 (unsigned)i + 1);
        HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, block, HTTPD_RESP_USE_STRLEN));
        char field[16];
        snprintf(field, sizeof(field), "hid%u_name", (unsigned)i + 1);
        HTTP_RETURN_ON_ERROR(send_input(req, "设备/屏幕名称", field, "text",
                                   settings->hid[i].name, "required maxlength=\"31\""));
        char width[8], height[8];
        snprintf(width, sizeof(width), "%u", settings->hid[i].width);
        snprintf(height, sizeof(height), "%u", settings->hid[i].height);
        snprintf(field, sizeof(field), "hid%u_width", (unsigned)i + 1);
        HTTP_RETURN_ON_ERROR(send_input(req, "屏幕宽度", field, "number", width,
                                   "required min=\"1\" max=\"32767\""));
        snprintf(field, sizeof(field), "hid%u_height", (unsigned)i + 1);
        HTTP_RETURN_ON_ERROR(send_input(req, "屏幕高度", field, "number", height,
                                   "required min=\"1\" max=\"32767\""));
        HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, "</div></div>", HTTPD_RESP_USE_STRLEN));
    }
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
        "</section><button type=\"submit\">保存并重启设备</button></form>"
        "<p class=\"hint\">配置保存在 NVS 中。SoftAP 地址固定为 192.168.1.100。</p>"
        "</body></html>", HTTPD_RESP_USE_STRLEN));
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t form_value(const char *form, const char *key,
                            char *value, size_t capacity)
{
    esp_err_t err = httpd_query_key_value(form, key, value, capacity);
    if (err != ESP_OK) return err;
    return url_decode(value) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static bool parse_u16(const char *text, uint16_t min, uint16_t max, uint16_t *value)
{
    char *end;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || number < min || number > max)
        return false;
    *value = (uint16_t)number;
    return true;
}

static bool valid_password(const char *password)
{
    size_t len = strlen(password);
    return len == 0 || (len >= 8 && len <= 63);
}

static bool valid_text(const char *text)
{
    if (!text[0]) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p < 0x20 || *p == 0x7f) return false;
    return true;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t save_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form length");
        return ESP_OK;
    }

    char form[2048];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int got = httpd_req_recv(req, form + received, req->content_len - received);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return ESP_FAIL;
        received += got;
    }
    form[received] = '\0';

    app_settings_t settings = {0};
    char port[8];
    if (form_value(form, "wifi_ssid", settings.wifi_ssid, sizeof(settings.wifi_ssid)) != ESP_OK ||
        form_value(form, "wifi_password", settings.wifi_password, sizeof(settings.wifi_password)) != ESP_OK ||
        form_value(form, "deskflow_host", settings.deskflow_host, sizeof(settings.deskflow_host)) != ESP_OK ||
        form_value(form, "deskflow_port", port, sizeof(port)) != ESP_OK ||
        form_value(form, "softap_ssid", settings.softap_ssid, sizeof(settings.softap_ssid)) != ESP_OK ||
        form_value(form, "softap_password", settings.softap_password, sizeof(settings.softap_password)) != ESP_OK ||
        form_value(form, "ble_device_name", settings.ble_device_name, sizeof(settings.ble_device_name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or oversized communication field");
        return ESP_OK;
    }

    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
        char field[16], width[8], height[8];
        snprintf(field, sizeof(field), "hid%u_name", (unsigned)i + 1);
        if (form_value(form, field, settings.hid[i].name,
                       sizeof(settings.hid[i].name)) != ESP_OK) goto invalid_hid;
        snprintf(field, sizeof(field), "hid%u_width", (unsigned)i + 1);
        if (form_value(form, field, width, sizeof(width)) != ESP_OK) goto invalid_hid;
        snprintf(field, sizeof(field), "hid%u_height", (unsigned)i + 1);
        if (form_value(form, field, height, sizeof(height)) != ESP_OK ||
            !parse_u16(width, 1, 32767, &settings.hid[i].width) ||
            !parse_u16(height, 1, 32767, &settings.hid[i].height) ||
            !valid_text(settings.hid[i].name)) goto invalid_hid;
    }

    struct in_addr host_addr;
    if (!valid_text(settings.wifi_ssid) ||
        !valid_password(settings.wifi_password) ||
        inet_pton(AF_INET, settings.deskflow_host, &host_addr) != 1 ||
        !parse_u16(port, 1, 65535, &settings.deskflow_port) ||
        !valid_text(settings.softap_ssid) ||
        !valid_password(settings.softap_password) ||
        !valid_text(settings.ble_device_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid communication settings");
        return ESP_OK;
    }
    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
        for (size_t j = i + 1; j < APP_MAX_HID_DEVICES; ++j) {
            if (strcmp(settings.hid[i].name, settings.hid[j].name) == 0) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "HID screen names must be unique");
                return ESP_OK;
            }
        }
    }

    esp_err_t err = app_settings_save(&settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save configuration: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "failed to save settings");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>配置已保存</title></head><body style=\"font-family:system-ui;padding:32px\">"
        "<h1>配置已保存</h1><p>新参数已经写入 NVS，设备正在重新启动…</p>"
        "<p>重启后请连接新的 SoftAP，然后访问 <code>http://192.168.1.100/</code>。</p>"
        "</body></html>");
    ESP_LOGI(TAG, "new configuration saved; restarting");
    xTaskCreate(restart_task, "config_restart", 2048, NULL, 5, NULL);
    return ESP_OK;

invalid_hid:
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "invalid HID name or screen dimensions");
    return ESP_OK;
}

esp_err_t config_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 6144;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;

    const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler
    };
    const httpd_uri_t save_uri = {
        .uri = "/save", .method = HTTP_POST, .handler = save_handler
    };
    if ((err = httpd_register_uri_handler(server, &root_uri)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &save_uri)) != ESP_OK) {
        httpd_stop(server);
        return err;
    }
    ESP_LOGI(TAG, "configuration page: http://192.168.1.100/");
    return ESP_OK;
}
