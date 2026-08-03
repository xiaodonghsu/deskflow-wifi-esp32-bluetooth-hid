#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"
#include "esp_err.h"

#define APP_SETTINGS_SSID_LEN   33
#define APP_SETTINGS_PASS_LEN   65
#define APP_SETTINGS_HOST_LEN   16
#define APP_SETTINGS_AP_SSID_LEN 33
#define APP_SETTINGS_BLE_NAME_LEN 30
#define APP_SETTINGS_HID_NAME_LEN 32

typedef struct {
    char name[APP_SETTINGS_HID_NAME_LEN];
    uint16_t width;
    uint16_t height;
    bool auto_lock;
} app_hid_settings_t;

typedef struct {
    char wifi_ssid[APP_SETTINGS_SSID_LEN];
    char wifi_password[APP_SETTINGS_PASS_LEN];
    char deskflow_host[APP_SETTINGS_HOST_LEN];
    uint16_t deskflow_port;
    char softap_ssid[APP_SETTINGS_AP_SSID_LEN];
    char softap_password[APP_SETTINGS_PASS_LEN];
    char usb_dhcp_server_ip[APP_SETTINGS_HOST_LEN];
    char ble_device_name[APP_SETTINGS_BLE_NAME_LEN];
    app_hid_settings_t hid[APP_MAX_HID_DEVICES];
} app_settings_t;

esp_err_t app_settings_init(void);
const app_settings_t *app_settings_get(void);
esp_err_t app_settings_save(const app_settings_t *settings);
