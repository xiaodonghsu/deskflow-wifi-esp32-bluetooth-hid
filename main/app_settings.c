#include "app_settings.h"
#include "app_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"

static app_settings_t s_settings;

static void set_defaults(void)
{
    memset(&s_settings, 0, sizeof(s_settings));
    strlcpy(s_settings.wifi_ssid, APP_WIFI_SSID, sizeof(s_settings.wifi_ssid));
    strlcpy(s_settings.wifi_password, APP_WIFI_PASSWORD, sizeof(s_settings.wifi_password));
    strlcpy(s_settings.deskflow_host, APP_DESKFLOW_SERVER_IP, sizeof(s_settings.deskflow_host));
    s_settings.deskflow_port = APP_DESKFLOW_PORT;
    strlcpy(s_settings.softap_ssid, APP_SOFTAP_SSID, sizeof(s_settings.softap_ssid));
    strlcpy(s_settings.softap_password, APP_SOFTAP_PASSWORD,
            sizeof(s_settings.softap_password));
    strlcpy(s_settings.usb_dhcp_server_ip, APP_USB_DHCP_SERVER_IP,
            sizeof(s_settings.usb_dhcp_server_ip));
    strlcpy(s_settings.ble_device_name, APP_BLE_DEVICE_NAME,
            sizeof(s_settings.ble_device_name));
    strlcpy(s_settings.hid[0].name, APP_HID_1_NAME, sizeof(s_settings.hid[0].name));
    s_settings.hid[0].width = APP_HID_1_WIDTH;
    s_settings.hid[0].height = APP_HID_1_HEIGHT;
    strlcpy(s_settings.hid[1].name, APP_HID_2_NAME, sizeof(s_settings.hid[1].name));
    s_settings.hid[1].width = APP_HID_2_WIDTH;
    s_settings.hid[1].height = APP_HID_2_HEIGHT;
    strlcpy(s_settings.hid[2].name, APP_HID_3_NAME, sizeof(s_settings.hid[2].name));
    s_settings.hid[2].width = APP_HID_3_WIDTH;
    s_settings.hid[2].height = APP_HID_3_HEIGHT;
}

static bool get_string(nvs_handle_t nvs, const char *key, char *value, size_t capacity)
{
    size_t len = capacity;
    if (nvs_get_str(nvs, key, value, &len) == ESP_OK) return true;
    value[capacity - 1] = '\0';
    return false;
}

esp_err_t app_settings_init(void)
{
    set_defaults();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("app_config", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    get_string(nvs, "ssid", s_settings.wifi_ssid, sizeof(s_settings.wifi_ssid));
    get_string(nvs, "pass", s_settings.wifi_password, sizeof(s_settings.wifi_password));
    get_string(nvs, "host", s_settings.deskflow_host, sizeof(s_settings.deskflow_host));
    get_string(nvs, "ap_ssid", s_settings.softap_ssid, sizeof(s_settings.softap_ssid));
    get_string(nvs, "ap_pass", s_settings.softap_password, sizeof(s_settings.softap_password));
    get_string(nvs, "usb_dhcp_ip", s_settings.usb_dhcp_server_ip,
               sizeof(s_settings.usb_dhcp_server_ip));
    get_string(nvs, "ble_name", s_settings.ble_device_name, sizeof(s_settings.ble_device_name));
    uint16_t port;
    if (nvs_get_u16(nvs, "port", &port) == ESP_OK && port != 0)
        s_settings.deskflow_port = port;

    char legacy_prefix[APP_SETTINGS_HID_NAME_LEN] = {0};
    bool have_legacy_prefix = get_string(nvs, "prefix", legacy_prefix,
                                         sizeof(legacy_prefix));
    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "hname%u", (unsigned)i);
        bool have_name = get_string(nvs, key, s_settings.hid[i].name,
                                    sizeof(s_settings.hid[i].name));
        if (!have_name && have_legacy_prefix) {
            char suffix[8];
            snprintf(suffix, sizeof(suffix), "%u", (unsigned)i + 1);
            size_t suffix_len = strlen(suffix);
            size_t prefix_len = strnlen(legacy_prefix,
                sizeof(s_settings.hid[i].name) - suffix_len - 1);
            memcpy(s_settings.hid[i].name, legacy_prefix, prefix_len);
            memcpy(s_settings.hid[i].name + prefix_len, suffix, suffix_len + 1);
        }
        snprintf(key, sizeof(key), "hw%u", (unsigned)i);
        nvs_get_u16(nvs, key, &s_settings.hid[i].width);
        snprintf(key, sizeof(key), "hh%u", (unsigned)i);
        nvs_get_u16(nvs, key, &s_settings.hid[i].height);
    }
    nvs_close(nvs);
    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_settings;
}

esp_err_t app_settings_save(const app_settings_t *settings)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("app_config", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    if ((err = nvs_set_str(nvs, "ssid", settings->wifi_ssid)) == ESP_OK)
        err = nvs_set_str(nvs, "pass", settings->wifi_password);
    if (err == ESP_OK) err = nvs_set_str(nvs, "host", settings->deskflow_host);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "port", settings->deskflow_port);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ap_ssid", settings->softap_ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ap_pass", settings->softap_password);
    if (err == ESP_OK) err = nvs_set_str(nvs, "usb_dhcp_ip",
                                         settings->usb_dhcp_server_ip);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ble_name", settings->ble_device_name);
    for (size_t i = 0; err == ESP_OK && i < APP_MAX_HID_DEVICES; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "hname%u", (unsigned)i);
        err = nvs_set_str(nvs, key, settings->hid[i].name);
        if (err == ESP_OK) {
            snprintf(key, sizeof(key), "hw%u", (unsigned)i);
            err = nvs_set_u16(nvs, key, settings->hid[i].width);
        }
        if (err == ESP_OK) {
            snprintf(key, sizeof(key), "hh%u", (unsigned)i);
            err = nvs_set_u16(nvs, key, settings->hid[i].height);
        }
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) s_settings = *settings;
    return err;
}
