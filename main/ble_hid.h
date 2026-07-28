#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ble_hid_init(void);
bool ble_hid_connected(void);
void ble_hid_keyboard(uint8_t modifiers, const uint8_t keys[6]);
void ble_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
