// Entry: init modules, start pipeline
#include "backend_client.h"
#include "board_audio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led.h"
#include "voice_session.h"
#include "wake_afe.h"
#include "wifi_connect.h"

static const char *TAG = "c_voice_client"; // prefix in LOGGING-files

static void wifi_retry_task(void *arg) {
    (void)arg;
    while (true) {
        if (!wifi_is_connected()) {
            esp_err_t ret = wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "wifi retry failed err=%s", esp_err_to_name(ret));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// Boot: init audio, wifi, backend, session, AFE/WakeNet. Then idle.
//
// Args: none
// Returns: void
void app_main(void) {
    ESP_LOGI(TAG, "starting demo-based C voice client");

    ESP_ERROR_CHECK(board_audio_init());           // 1. init audio codecs
    esp_err_t ret = status_led_init();             // 2. init RGB ring if available
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "status LED unavailable err=%s; continuing", esp_err_to_name(ret));
    }

    ret = wifi_connect();                          // 3. connect wifi if available
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi unavailable at boot err=%s; continuing", esp_err_to_name(ret));
    }
    xTaskCreate(wifi_retry_task, "wifi_retry", 4096, NULL, 3, NULL);

    ret = backend_client_health_check();           // 4. check backend if reachable
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "backend health unavailable at boot err=%s; continuing", esp_err_to_name(ret));
    }

    ESP_ERROR_CHECK(voice_session_init());         // 5. init recorder
    ESP_ERROR_CHECK(wake_afe_start(voice_session_on_audio)); // 6. start AFE/WakeNet

    ESP_LOGI(TAG, "say Alexa near the board");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));           // idle loop
    }
}
