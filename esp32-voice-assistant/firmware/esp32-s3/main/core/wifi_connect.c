#include "wifi_connect.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi_connect";
static EventGroupHandle_t s_wifi_events;
static int s_retry_count;
static bool s_initialized;
static bool s_connected;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_retry_count < CONFIG_VOICE_WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "disconnected; retry %d/%d", s_retry_count, CONFIG_VOICE_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        s_retry_count = 0;
        ESP_LOGI(TAG, "connected ip=" IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t init_wifi_once(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    if (strlen(CONFIG_VOICE_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "CONFIG_VOICE_WIFI_SSID is empty; run idf.py menuconfig");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "init nvs");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "init netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "create event loop");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "init wifi");

    s_wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_events != NULL, ESP_ERR_NO_MEM, TAG, "create event group");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "register wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG, "register ip handler");

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_VOICE_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_VOICE_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi");

    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_connect(void) {
    ESP_RETURN_ON_ERROR(init_wifi_once(), TAG, "init wifi once");

    if (s_connected) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "connecting to ssid=%s", CONFIG_VOICE_WIFI_SSID);
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(CONFIG_VOICE_WIFI_CONNECT_TIMEOUT_MS));

    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    if ((bits & WIFI_FAIL_BIT) != 0) {
        ESP_LOGE(TAG, "failed to connect to configured ssid");
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "connection timed out after %d ms", CONFIG_VOICE_WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void) {
    return s_connected;
}
