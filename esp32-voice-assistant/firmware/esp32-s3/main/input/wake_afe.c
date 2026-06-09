#include "wake_afe.h"

#include <assert.h>
#include "board_audio.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "model_path.h"

static const char *TAG = "wake_afe";

static const esp_afe_sr_iface_t *afe_handle;
static wake_afe_audio_cb_t audio_callback; // session callback
static volatile bool paused; // pause flag

// Feed task: read audio from mic, feed to AFE. Runs on core 0.
//
// Args: arg - AFE data pointer
// Returns: void
static void feed_task(void *arg) {
    esp_afe_sr_data_t *afe_data = arg;
    int chunks = afe_handle->get_feed_chunksize(afe_data);
    int channels = afe_handle->get_feed_channel_num(afe_data);
    assert(channels == board_audio_feed_channels());

    int byte_len = chunks * channels * sizeof(int16_t);
    int16_t *buffer = heap_caps_malloc(byte_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // PSRAM buffer
    assert(buffer);

    while (true) {
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        esp_err_t err = board_audio_read_feed(buffer, byte_len); // read from mic
        if (err == ESP_OK) {
            afe_handle->feed(afe_data, buffer); // feed to AFE
        } else {
            ESP_LOGE(TAG, "audio read failed: %s", esp_err_to_name(err));
        }
    }
}

// Fetch task: process audio from AFE, detect wakeword, send to session. Runs on core 1.
//
// Args: arg - AFE data pointer
// Returns: void
static void fetch_task(void *arg) {
    esp_afe_sr_data_t *afe_data = arg;

    while (true) {
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        afe_fetch_result_t *res = afe_handle->fetch(afe_data); // fetch processed audio
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "afe fetch failed");
            continue;
        }

        int wake_state = res->wakeup_state;
        if (res->raw_data_channels > 1 && wake_state == WAKENET_CHANNEL_VERIFIED) {
            wake_state = WAKENET_DETECTED; // multi-channel confirmed
        }

        if (audio_callback && res->data && res->data_size > 0) {
            audio_callback(res->data, res->data_size / sizeof(int16_t), wake_state); // send to session
        }
    }
}

// Pause AFE feed/fetch tasks (used during export).
//
// Args: none
// Returns: void
void wake_afe_pause(void) {
    paused = true;
}

void wake_afe_resume(void) {
    paused = false;
}

// Start AFE/WakeNet: load models, init AFE, start feed/fetch tasks.
//
// Args: callback - audio callback for processed frames
// Returns: ESP_OK on success, error code otherwise
esp_err_t wake_afe_start(wake_afe_audio_cb_t callback) {
    audio_callback = callback;

    srmodel_list_t *models = esp_srmodel_init("model"); // load from flash
    if (!models || models->num <= 0) {
        ESP_LOGE(TAG, "no models loaded from partition 'model'");
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < models->num; i++) {
        ESP_LOGI(TAG, "loaded model[%d]=%s", i, models->model_name[i]);
    }

    afe_config_t *cfg = afe_config_init(board_audio_input_format(), models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    cfg->ns_init = false; // no noise suppression
    cfg->vad_init = false; // no VAD
    afe_handle = esp_afe_handle_from_config(cfg);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(cfg);
    afe_config_free(cfg);

    xTaskCreatePinnedToCore(fetch_task, "afe_fetch", 8 * 1024, afe_data, 5, NULL, 1); // core 1: 8KB stack
    xTaskCreatePinnedToCore(feed_task, "afe_feed", 8 * 1024, afe_data, 5, NULL, 0); // core 0: 8KB stack
    ESP_LOGI(TAG, "started: input_format=%s", board_audio_input_format());
    return ESP_OK;
}
