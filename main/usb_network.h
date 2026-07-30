#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"

esp_err_t usb_network_start(esp_netif_t *wifi_ap_netif);
bool usb_network_attached(void);
esp_err_t usb_network_peer_ip(char *address, size_t capacity);
