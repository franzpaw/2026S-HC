// Output adapter. Keeps voice_session transport-free.
#include "capture_output.h"

#include <string.h>

#include "backend_client.h"
#include "board_audio.h"
#include "esp_log.h"
#include "wav.h"

static const char *TAG = "capture_output";

typedef struct {
    esp_err_t result;
} capture_output_ctx_t;

static void on_response_audio(const uint8_t *audio_data, size_t audio_size, const char *format, void *user_ctx) {
    capture_output_ctx_t *ctx = user_ctx;
    ESP_LOGI(TAG, "response audio received bytes=%u format=%s", (unsigned)audio_size, format);

    if (format == NULL || strcmp(format, "wav") != 0) {
        if (ctx != NULL) {
            ctx->result = ESP_ERR_NOT_SUPPORTED;
        }
        return;
    }

    wav_pcm16_view_t pcm = {0};
    esp_err_t ret = wav_parse_pcm16_mono(audio_data, audio_size, &pcm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wav parse failed err=0x%x", (unsigned)ret);
        if (ctx != NULL) {
            ctx->result = ret;
        }
        return;
    }

    ESP_LOGI(TAG,
             "wav parsed rate=%u channels=%u bits=%u pcm_bytes=%u",
             (unsigned)pcm.sample_rate_hz,
             (unsigned)pcm.channels,
             (unsigned)pcm.bits_per_sample,
             (unsigned)(pcm.sample_count * sizeof(int16_t)));

    ESP_LOGI(TAG, "playback start samples=%u", (unsigned)pcm.sample_count);
    ret = board_audio_write_pcm16_mono(pcm.samples, pcm.sample_count, pcm.sample_rate_hz);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "playback failed err=0x%x", (unsigned)ret);
        if (ctx != NULL) {
            ctx->result = ret;
        }
        return;
    }
    ESP_LOGI(TAG, "playback done");
}

// Send the finished recording to the backend.
esp_err_t capture_output_send_wav(const uint8_t *wav, size_t wav_len) {
    capture_output_ctx_t ctx = {.result = ESP_OK};
    esp_err_t ret = backend_client_chat_audio(wav, wav_len, on_response_audio, &ctx);
    if (ret != ESP_OK) {
        return ret;
    }
    return ctx.result;
}
