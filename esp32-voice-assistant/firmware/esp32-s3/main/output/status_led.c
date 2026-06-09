#include "status_led.h"

#include "esp_check.h"
#include "led_strip.h"

#define STATUS_LED_GPIO 38
#define STATUS_LED_COUNT 7
#define REC_R 180
#define REC_G 20
#define REC_B 80

static const char *TAG = "status_led";
static led_strip_handle_t strip;

esp_err_t status_led_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = STATUS_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip), TAG, "led strip init failed");
    return status_led_off();
}

esp_err_t status_led_recording_on(void) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < STATUS_LED_COUNT; i++) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(strip, i, REC_R, REC_G, REC_B), TAG, "set recording led failed");
    }
    return led_strip_refresh(strip);
}

esp_err_t status_led_off(void) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    return led_strip_clear(strip);
}
