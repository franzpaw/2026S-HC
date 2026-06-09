#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Connects station Wi-Fi and logs the assigned IP address.
esp_err_t wifi_connect(void);

// Current station connection state.
bool wifi_is_connected(void);
