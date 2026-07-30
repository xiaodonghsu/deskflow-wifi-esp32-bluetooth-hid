#include "wifi.h"
#include "app_config.h"
#include "app_settings.h"
#include "usb_network.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_events;
static unsigned s_retry_count;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
#if APP_WIFI_MAX_RETRY == 0
        ESP_LOGW(TAG, "disconnected; reconnecting");
        esp_wifi_connect();
#else
        if (s_retry_count++ < APP_WIFI_MAX_RETRY) {
            ESP_LOGW(TAG, "disconnected; reconnecting");
            esp_wifi_connect();
        }
#endif
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_start(void)
{
    const app_settings_t *settings = app_settings_get();
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;
    esp_netif_inherent_config_t ap_base = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    ap_base.flags = ESP_NETIF_FLAG_AUTOUP;
    ap_base.ip_info = NULL;
    esp_netif_t *ap_netif = esp_netif_create_wifi(WIFI_IF_AP, &ap_base);
    if (!ap_netif) return ESP_FAIL;
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_ap_handlers());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, settings->wifi_ssid,
            sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, settings->wifi_password,
            sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = settings->wifi_password[0]
        ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, settings->softap_ssid,
            sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, settings->softap_password,
            sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(settings->softap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = settings->softap_password[0]
        ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(usb_network_start(ap_netif));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "configuration AP \"%s\" at %s",
             settings->softap_ssid, settings->usb_dhcp_server_ip);
    return ESP_OK;
}

void wifi_wait_connected(void)
{
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

bool wifi_connected(void)
{
    return s_wifi_events &&
        (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}
