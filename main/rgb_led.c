#include "rgb_led.h"
#include "app_config.h"

#include <stdint.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

#define RGB_LED_RMT_RESOLUTION_HZ (10 * 1000 * 1000)

static const char *TAG = "rgb_led";
static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_lock;

esp_err_t rgb_led_init(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = APP_RGB_LED_GPIO,
        .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RGB_LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG,
                        "failed to create RGB LED mutex");
    ESP_RETURN_ON_ERROR(
        led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip),
        TAG, "failed to initialize RGB LED on GPIO %d", APP_RGB_LED_GPIO);
    ESP_RETURN_ON_ERROR(led_strip_clear(s_strip), TAG,
                        "failed to turn RGB LED off");
    ESP_LOGI(TAG, "RGB LED ready on GPIO %d", APP_RGB_LED_GPIO);
    return ESP_OK;
}

void rgb_led_show_device(size_t device_index)
{
    static const uint8_t colors[][3] = {
        {APP_RGB_LED_BRIGHTNESS, 0, 0},
        {0, APP_RGB_LED_BRIGHTNESS, 0},
        {0, 0, APP_RGB_LED_BRIGHTNESS},
    };
    if (!s_strip || device_index >= sizeof(colors) / sizeof(colors[0]))
        return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = led_strip_set_pixel(
        s_strip, 0, colors[device_index][0], colors[device_index][1],
        colors[device_index][2]);
    if (err == ESP_OK) err = led_strip_refresh(s_strip);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "failed to show device %u color: %s",
                 (unsigned)device_index + 1, esp_err_to_name(err));
}

void rgb_led_off(void)
{
    if (!s_strip) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = led_strip_clear(s_strip);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "failed to turn RGB LED off: %s", esp_err_to_name(err));
}
