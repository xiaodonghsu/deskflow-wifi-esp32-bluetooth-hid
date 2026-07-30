#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t rgb_led_init(void);
void rgb_led_show_device(size_t device_index);
void rgb_led_off(void);
