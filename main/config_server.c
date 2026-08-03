#include "config_server.h"
#include "app_config.h"
#include "app_settings.h"
#include "ble_hid.h"
#include "usb_network.h"
#include "wifi.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_flash.h"
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
    "h1{font-size:1.7rem;margin:0}.card{background:#fff;border-radius:14px;"
    "padding:20px;margin:14px 0;box-shadow:0 5px 20px #18315318}"
    "nav{display:flex;gap:8px;margin:18px 0;flex-wrap:wrap}nav a{padding:9px 14px;"
    "border-radius:9px;background:#dce7f8;color:#2457a7;text-decoration:none;font-weight:700}"
    "nav a.active{background:#2463d4;color:#fff}"
    "h2{font-size:1.15rem;margin:0 0 14px;color:#2457a7}.grid{display:grid;"
    "grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}"
    "label{display:flex;flex-direction:column;gap:6px;font-size:.9rem;font-weight:600}"
    "input{box-sizing:border-box;width:100%;padding:10px 12px;border:1px solid #b8c3d4;"
    "border-radius:8px;background:#fff;color:#172033;font:inherit}"
    ".toggle{grid-column:1/-1;flex-direction:row;align-items:center}.toggle input{width:auto}"
    ".wide{grid-column:1/-1}.hid{border-top:1px solid #dce3ed;padding-top:14px;margin-top:14px}"
    ".hid:first-of-type{border-top:0;padding-top:0}.hint{font-size:.84rem;color:#61708a}"
    "button{width:100%;border:0;border-radius:10px;padding:13px;background:#2463d4;"
    "color:#fff;font-size:1rem;font-weight:700;cursor:pointer}"
    ".peer{display:flex;align-items:center;justify-content:space-between;gap:12px;"
    "margin-top:12px;padding:10px 12px;border-radius:8px;background:#f3f6fa}"
    ".peer button{width:auto;padding:8px 12px;background:#c83232;font-size:.88rem}"
    ".status{font-size:.88rem}.online{color:#16803c}.offline{color:#68758a}"
    ".info{display:grid;grid-template-columns:minmax(120px,1fr) 2fr;gap:10px 18px}"
    ".info dt{color:#61708a}.info dd{margin:0;font-weight:600;overflow-wrap:anywhere}"
    "@media(max-width:600px){.grid{grid-template-columns:1fr}.wide{grid-column:auto}}"
    "@media(prefers-color-scheme:dark){body{background:#111827;color:#e5e7eb}.card{background:#1f2937}"
    "input{background:#111827;color:#e5e7eb;border-color:#4b5563}.hint,.info dt{color:#9ca3af}"
    ".peer{background:#111827}nav a{background:#374151;color:#bfdbfe}}"
    "</style></head><body><h1>Deskflow Wi-Fi HID 配置</h1>";

static esp_err_t send_nav(httpd_req_t *req, const char *active)
{
    char nav[512];
    int len = snprintf(nav, sizeof(nav),
        "<nav><a class=\"%s\" href=\"/\">概览</a>"
        "<a class=\"%s\" href=\"/communication\">通信设置</a>"
        "<a class=\"%s\" href=\"/hid\">HID 设置</a></nav>",
        strcmp(active, "overview") == 0 ? "active" : "",
        strcmp(active, "communication") == 0 ? "active" : "",
        strcmp(active, "hid") == 0 ? "active" : "");
    if (len < 0 || (size_t)len >= sizeof(nav)) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nav, len);
}

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
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t internal_total =
        heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    bool has_psram = psram_total > 0;
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t flash_size = 0;
    bool has_flash_size = esp_flash_get_size(NULL, &flash_size) == ESP_OK;
    char flash_text[48];
    char psram_text[64];
    if (has_flash_size)
        snprintf(flash_text, sizeof(flash_text), "%u MB（%u 字节）",
                 (unsigned)(flash_size / (1024 * 1024)), (unsigned)flash_size);
    else
        strlcpy(flash_text, "读取失败", sizeof(flash_text));
    if (has_psram)
        snprintf(psram_text, sizeof(psram_text), "%u KB（可用 %u KB）",
                 (unsigned)(psram_total / 1024), (unsigned)(psram_free / 1024));
    else
        strlcpy(psram_text, "未启用或未检测到", sizeof(psram_text));

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN));
    HTTP_RETURN_ON_ERROR(send_nav(req, "overview"));
    char overview[1600];
    int len = snprintf(overview, sizeof(overview),
        "<section class=\"card\"><h2>设备概览</h2><dl class=\"info\">"
        "<dt>项目</dt><dd>%s</dd><dt>固件版本</dt><dd>%s</dd>"
        "<dt>编译时间</dt><dd>%s %s</dd><dt>IDF 版本</dt><dd>%s</dd>"
        "<dt>芯片</dt><dd>ESP32-S3 rev %u.%u，%u 核</dd>"
        "<dt>Flash 容量</dt><dd>%s</dd><dt>PSRAM</dt><dd>%s</dd>"
        "<dt>内部 RAM</dt><dd>%u KB（可用 %u KB）</dd>"
        "<dt>可用内存</dt><dd>%u KB</dd><dt>历史最低可用内存</dt><dd>%u KB</dd>"
        "<dt>最大连续内存块</dt><dd>%u KB</dd></dl></section>",
        app->project_name, app->version, app->date, app->time, app->idf_ver,
        chip.revision / 100, chip.revision % 100, chip.cores,
        flash_text, psram_text, (unsigned)(internal_total / 1024),
        (unsigned)(internal_free / 1024),
        (unsigned)(free_heap / 1024), (unsigned)(min_heap / 1024),
        (unsigned)(largest / 1024));
    if (len < 0 || (size_t)len >= sizeof(overview)) return ESP_FAIL;
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, overview, len));

    wifi_sta_status_t sta;
    char network[1024];
    if (wifi_sta_status(&sta)) {
        char escaped_ssid[160];
        html_escape(sta.ssid, escaped_ssid, sizeof(escaped_ssid));
        len = snprintf(network, sizeof(network),
            "<section class=\"card\"><h2>STA 网络</h2><dl class=\"info\">"
            "<dt>状态</dt><dd class=\"online\">已连接</dd>"
            "<dt>SSID</dt><dd>%s</dd><dt>信号</dt><dd>%d dBm</dd>"
            "<dt>IP 地址</dt><dd>%s</dd><dt>子网掩码</dt><dd>%s</dd>"
            "<dt>网关</dt><dd>%s</dd></dl></section>",
            escaped_ssid, sta.rssi, sta.ip, sta.netmask, sta.gateway);
    } else {
        len = snprintf(network, sizeof(network),
            "<section class=\"card\"><h2>STA 网络</h2>"
            "<p class=\"offline\">未连接或尚未获取 IP 地址</p></section>");
    }
    if (len < 0 || (size_t)len >= sizeof(network)) return ESP_FAIL;
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, network, len));

    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
        "<section class=\"card\"><h2>已连接设备</h2>",
        HTTPD_RESP_USE_STRLEN));
    wifi_ap_client_t ap_clients[4];
    size_t ap_count = wifi_ap_clients(ap_clients, 4);
    if (ap_count == 0) {
        HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
            "<p class=\"offline\">SoftAP：暂无连接设备</p>",
            HTTPD_RESP_USE_STRLEN));
    } else {
        for (size_t i = 0; i < ap_count; ++i) {
            len = snprintf(network, sizeof(network),
                "<dl class=\"info\"><dt>SoftAP 设备 %u</dt><dd>%s</dd>"
                "<dt>MAC 地址</dt><dd>%s</dd></dl>",
                (unsigned)i + 1, ap_clients[i].ip, ap_clients[i].mac);
            if (len < 0 || (size_t)len >= sizeof(network)) return ESP_FAIL;
            HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, network, len));
        }
    }

    char usb_ip[16], usb_mac[18];
    if (usb_network_peer_info(usb_ip, sizeof(usb_ip),
                              usb_mac, sizeof(usb_mac))) {
        len = snprintf(network, sizeof(network),
            "<dl class=\"info\"><dt>USB-NCM 主机</dt><dd>%s</dd>"
            "<dt>MAC 地址</dt><dd>%s</dd></dl>", usb_ip, usb_mac);
        if (len < 0 || (size_t)len >= sizeof(network)) return ESP_FAIL;
        HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, network, len));
    } else {
        HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
            "<p class=\"offline\">USB-NCM：未连接</p>",
            HTTPD_RESP_USE_STRLEN));
    }
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
        "</section></body></html>", HTTPD_RESP_USE_STRLEN));
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t communication_handler(httpd_req_t *req)
{
    const app_settings_t *settings = app_settings_get();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN));
    HTTP_RETURN_ON_ERROR(send_nav(req, "communication"));
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
        "<form method=\"post\" action=\"/save-communication\">"
        "<section class=\"card\"><h2>通信设置</h2><div class=\"grid\">",
        HTTPD_RESP_USE_STRLEN));
    HTTP_RETURN_ON_ERROR(send_input(req, "Wi-Fi SSID", "wifi_ssid", "text",
                               settings->wifi_ssid, "required maxlength=\"32\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "Wi-Fi 密码", "wifi_password", "password",
                               settings->wifi_password, "maxlength=\"63\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "Deskflow Server IP（STA 备用）", "deskflow_host", "text",
                               settings->deskflow_host, "required inputmode=\"decimal\""));
    char port[8];
    snprintf(port, sizeof(port), "%u", settings->deskflow_port);
    HTTP_RETURN_ON_ERROR(send_input(req, "Deskflow Server Port", "deskflow_port", "number",
                               port, "required min=\"1\" max=\"65535\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "SoftAP SSID", "softap_ssid", "text",
                               settings->softap_ssid, "required maxlength=\"32\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "SoftAP 密码（可选）", "softap_password", "password",
                               settings->softap_password, "maxlength=\"63\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "USB/SoftAP DHCP Server IP",
                               "usb_dhcp_server_ip", "text",
                               settings->usb_dhcp_server_ip,
                               "required inputmode=\"decimal\""));
    HTTP_RETURN_ON_ERROR(send_input(req, "BLE 设备名称", "ble_device_name", "text",
                               settings->ble_device_name,
                               "required maxlength=\"29\" class=\"wide\""));
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
        "</div><p class=\"hint\">密码留空表示开放网络；非空密码长度必须为 8–63 个字符。</p>"
        "</section><button type=\"submit\">保存并重启设备</button></form></body></html>",
        HTTPD_RESP_USE_STRLEN));
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t hid_handler(httpd_req_t *req)
{
    const app_settings_t *settings = app_settings_get();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN));
    HTTP_RETURN_ON_ERROR(send_nav(req, "hid"));
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req,
        "<form method=\"post\" action=\"/save-hid\">"
        "<section class=\"card\"><h2>HID 设置</h2>",
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
        char toggle[192];
        int toggle_len = snprintf(toggle, sizeof(toggle),
            "<label class=\"toggle\"><input type=\"checkbox\" name=\"hid%u_auto_lock\" "
            "value=\"1\" %s>自动锁屏（鼠标离开屏幕时）</label>",
            (unsigned)i + 1, settings->hid[i].auto_lock ? "checked" : "");
        if (toggle_len < 0 || (size_t)toggle_len >= sizeof(toggle)) return ESP_FAIL;
        HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, toggle, toggle_len));
        bool connected = false;
        char address[18];
        bool paired = ble_hid_peer_info(i, &connected, address, sizeof(address));
        char peer[480];
        int peer_len;
        if (paired) {
            peer_len = snprintf(peer, sizeof(peer),
                "</div><div class=\"peer\"><span class=\"status %s\">"
                "%s · %s</span><button type=\"submit\" name=\"slot\" value=\"%u\" "
                "formaction=\"/delete-peer\" formmethod=\"post\" formnovalidate "
                "onclick=\"return confirm('确定删除 HID 设备 %u 的蓝牙绑定吗？')\">"
                "删除绑定</button></div></div>",
                connected ? "online" : "offline",
                connected ? "已连接" : "已绑定（未连接）", address,
                (unsigned)i, (unsigned)i + 1);
        } else {
            peer_len = snprintf(peer, sizeof(peer),
                "</div><div class=\"peer\"><span class=\"status offline\">"
                "未绑定，可添加新设备</span></div></div>");
        }
        if (peer_len < 0 || (size_t)peer_len >= sizeof(peer)) return ESP_FAIL;
        HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, peer, peer_len));
    }
    char footer[320];
    int footer_len = snprintf(footer, sizeof(footer),
        "</section><button type=\"submit\">保存 HID 设置并重启设备</button></form>"
        "<p class=\"hint\">配置保存在 NVS 中。配置页面地址为 "
        "<code>http://%s/</code>；USB Deskflow 地址自动使用 DHCP Server IP + 1。</p>"
        "</body></html>", settings->usb_dhcp_server_ip);
    if (footer_len < 0 || (size_t)footer_len >= sizeof(footer))
        return ESP_FAIL;
    HTTP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, footer, footer_len));
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

static esp_err_t read_form(httpd_req_t *req, char *form, size_t capacity)
{
    form[0] = '\0';
    if (req->content_len <= 0 || (size_t)req->content_len >= capacity) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form length");
        return ESP_OK;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int got = httpd_req_recv(req, form + received, req->content_len - received);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return ESP_FAIL;
        received += got;
    }
    form[received] = '\0';
    return ESP_OK;
}

static esp_err_t save_communication_handler(httpd_req_t *req)
{
    char form[1536];
    esp_err_t read_err = read_form(req, form, sizeof(form));
    if (read_err != ESP_OK || form[0] == '\0') return read_err;

    app_settings_t settings = *app_settings_get();
    char port[8];
    if (form_value(form, "wifi_ssid", settings.wifi_ssid, sizeof(settings.wifi_ssid)) != ESP_OK ||
        form_value(form, "wifi_password", settings.wifi_password, sizeof(settings.wifi_password)) != ESP_OK ||
        form_value(form, "deskflow_host", settings.deskflow_host, sizeof(settings.deskflow_host)) != ESP_OK ||
        form_value(form, "deskflow_port", port, sizeof(port)) != ESP_OK ||
        form_value(form, "softap_ssid", settings.softap_ssid, sizeof(settings.softap_ssid)) != ESP_OK ||
        form_value(form, "softap_password", settings.softap_password, sizeof(settings.softap_password)) != ESP_OK ||
        form_value(form, "usb_dhcp_server_ip", settings.usb_dhcp_server_ip,
                   sizeof(settings.usb_dhcp_server_ip)) != ESP_OK ||
        form_value(form, "ble_device_name", settings.ble_device_name, sizeof(settings.ble_device_name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or oversized communication field");
        return ESP_OK;
    }

    struct in_addr host_addr;
    struct in_addr dhcp_addr;
    bool valid_dhcp_ip =
        inet_pton(AF_INET, settings.usb_dhcp_server_ip, &dhcp_addr) == 1 &&
        (ntohl(dhcp_addr.s_addr) & 0xff) >= 1 &&
        (ntohl(dhcp_addr.s_addr) & 0xff) <= 249;
    if (!valid_text(settings.wifi_ssid) ||
        !valid_password(settings.wifi_password) ||
        inet_pton(AF_INET, settings.deskflow_host, &host_addr) != 1 ||
        !parse_u16(port, 1, 65535, &settings.deskflow_port) ||
        !valid_text(settings.softap_ssid) ||
        !valid_password(settings.softap_password) ||
        !valid_dhcp_ip ||
        !valid_text(settings.ble_device_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid communication settings");
        return ESP_OK;
    }
    esp_err_t err = app_settings_save(&settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save configuration: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "failed to save settings");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char response[640];
    int response_len = snprintf(response, sizeof(response),
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>配置已保存</title></head><body style=\"font-family:system-ui;padding:32px\">"
        "<h1>配置已保存</h1><p>新参数已经写入 NVS，设备正在重新启动…</p>"
        "<p>重启后请连接新的 SoftAP，然后访问 <code>http://%s/communication</code>。</p>"
        "</body></html>", settings.usb_dhcp_server_ip);
    if (response_len < 0 || (size_t)response_len >= sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "response generation failed");
        return ESP_OK;
    }
    httpd_resp_send(req, response, response_len);
    ESP_LOGI(TAG, "new configuration saved; restarting");
    xTaskCreate(restart_task, "config_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t save_hid_handler(httpd_req_t *req)
{
    char form[1024];
    esp_err_t read_err = read_form(req, form, sizeof(form));
    if (read_err != ESP_OK || form[0] == '\0') return read_err;

    app_settings_t settings = *app_settings_get();
    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
        char field[16], width[8], height[8];
        snprintf(field, sizeof(field), "hid%u_name", (unsigned)i + 1);
        if (form_value(form, field, settings.hid[i].name,
                       sizeof(settings.hid[i].name)) != ESP_OK) goto invalid;
        snprintf(field, sizeof(field), "hid%u_width", (unsigned)i + 1);
        if (form_value(form, field, width, sizeof(width)) != ESP_OK) goto invalid;
        snprintf(field, sizeof(field), "hid%u_height", (unsigned)i + 1);
        if (form_value(form, field, height, sizeof(height)) != ESP_OK ||
            !parse_u16(width, 1, 32767, &settings.hid[i].width) ||
            !parse_u16(height, 1, 32767, &settings.hid[i].height) ||
            !valid_text(settings.hid[i].name)) goto invalid;
        snprintf(field, sizeof(field), "hid%u_auto_lock", (unsigned)i + 1);
        char checked[2];
        settings.hid[i].auto_lock =
            form_value(form, field, checked, sizeof(checked)) == ESP_OK &&
            strcmp(checked, "1") == 0;
    }
    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i)
        for (size_t j = i + 1; j < APP_MAX_HID_DEVICES; ++j)
            if (strcmp(settings.hid[i].name, settings.hid[j].name) == 0) goto invalid;

    esp_err_t err = app_settings_save(&settings);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "failed to save HID settings");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<body style=\"font-family:system-ui;padding:32px\"><h1>HID 设置已保存</h1>"
        "<p>设备正在重新启动…</p></body></html>", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "config_restart", 2048, NULL, 5, NULL);
    return ESP_OK;

invalid:
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "invalid or duplicate HID settings");
    return ESP_OK;
}

static esp_err_t delete_peer_handler(httpd_req_t *req)
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

    char slot_text[8];
    uint16_t slot;
    if (form_value(form, "slot", slot_text, sizeof(slot_text)) != ESP_OK ||
        !parse_u16(slot_text, 0, APP_MAX_HID_DEVICES - 1, &slot)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid HID slot");
        return ESP_OK;
    }

    esp_err_t err = ble_hid_remove_peer(slot);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "failed to remove HID target %u: %s",
                 slot + 1, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "failed to remove HID peer");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
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
    const httpd_uri_t communication_uri = {
        .uri = "/communication", .method = HTTP_GET, .handler = communication_handler
    };
    const httpd_uri_t hid_uri = {
        .uri = "/hid", .method = HTTP_GET, .handler = hid_handler
    };
    const httpd_uri_t save_communication_uri = {
        .uri = "/save-communication", .method = HTTP_POST,
        .handler = save_communication_handler
    };
    const httpd_uri_t save_hid_uri = {
        .uri = "/save-hid", .method = HTTP_POST, .handler = save_hid_handler
    };
    const httpd_uri_t delete_peer_uri = {
        .uri = "/delete-peer", .method = HTTP_POST, .handler = delete_peer_handler
    };
    if ((err = httpd_register_uri_handler(server, &root_uri)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &communication_uri)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &hid_uri)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &save_communication_uri)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &save_hid_uri)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &delete_peer_uri)) != ESP_OK) {
        httpd_stop(server);
        return err;
    }
    ESP_LOGI(TAG, "configuration page: http://%s/",
             app_settings_get()->usb_dhcp_server_ip);
    return ESP_OK;
}
