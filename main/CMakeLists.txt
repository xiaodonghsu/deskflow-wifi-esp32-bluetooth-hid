idf_component_register(
    SRCS "main.c" "wifi.c" "ble_hid.c" "deskflow.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif bt
)
