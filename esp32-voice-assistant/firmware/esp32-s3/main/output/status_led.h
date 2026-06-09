#pragma once

#include "esp_err.h"

// Board RGB ring recording indicator.
esp_err_t status_led_init(void);
esp_err_t status_led_recording_on(void);
esp_err_t status_led_off(void);
