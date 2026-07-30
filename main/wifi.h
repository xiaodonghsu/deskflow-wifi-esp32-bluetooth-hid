#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_start(void);
void wifi_wait_connected(void);
bool wifi_connected(void);
