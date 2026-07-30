#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    char ip[16];
    char netmask[16];
    char gateway[16];
    int8_t rssi;
} wifi_sta_status_t;

typedef struct {
    char ip[16];
    char mac[18];
} wifi_ap_client_t;

esp_err_t wifi_start(void);
void wifi_wait_connected(void);
bool wifi_connected(void);
bool wifi_sta_status(wifi_sta_status_t *status);
size_t wifi_ap_clients(wifi_ap_client_t *clients, size_t capacity);
