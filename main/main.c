#include "app_settings.h"
#include "ble_hid.h"
#include "config_server.h"
#include "deskflow.h"
#include "wifi.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(app_settings_init());
    ESP_ERROR_CHECK(ble_hid_init());
    ESP_ERROR_CHECK(wifi_start());
    ESP_ERROR_CHECK(config_server_start());
    ESP_ERROR_CHECK(deskflow_start());
    ESP_LOGI(TAG, "Deskflow Wi-Fi to NimBLE HID bridge started");
}
