#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ble_hid_init(void);
bool ble_hid_connected(size_t target);
void ble_hid_keyboard(size_t target, uint8_t modifiers, const uint8_t keys[6]);
void ble_hid_mouse(size_t target, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
