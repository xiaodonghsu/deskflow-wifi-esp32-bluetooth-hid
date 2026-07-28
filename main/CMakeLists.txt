idf_component_register(
    SRCS "main.c" "app_settings.c" "config_server.c" "wifi.c" "ble_hid.c" "deskflow.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_server bt
)
