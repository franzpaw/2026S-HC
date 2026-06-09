// Voice session owns capture policy, not hardware or transport.
#include "voice_session.h"

#include <stdbool.h>
#include <stdlib.h>
#include "board_audio.h"
#include "capture_output.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "recorder.h"
#include "status_led.h"
#include "wake_afe.h"
#include "wav.h"

#define PRE_ROLL_MS 1200
#define SILENCE_STOP_MS 2500
#define MAX_RECORDING_MS 10000
#define SILENCE_THRESHOLD 60

static const char *TAG = "voice_session";
static bool triggered;
static bool exporting;

static void set_recording_led(bool on) {
    esp_err_t ret = on ? status_led_recording_on() : status_led_off();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "status LED update failed err=0x%x", (unsigned)ret);
    }
}

// Init recorder with pre-roll, silence stop, max duration.
//
// Args: none
// Returns: ESP_OK on success, error code otherwise
esp_err_t voice_session_init(void) {
    recorder_config_t config = {
        .sample_rate_hz = BOARD_AUDIO_SAMPLE_RATE,
        .pre_roll_ms = PRE_ROLL_MS, // 1.2s buffer before wakeword
        .silence_stop_ms = SILENCE_STOP_MS, // stop after 2.5s silence
        .max_recording_ms = MAX_RECORDING_MS, // hard limit 10s
        .silence_threshold = SILENCE_THRESHOLD,
    };
    return recorder_init(config);
}

// Export recording: pause AFE, convert PCM to WAV, send to output.
//
// Args: none
// Returns: void
static void reset_for_next_turn(void) {
    recorder_reset();
    triggered = false;
    exporting = false;
    wake_afe_resume();
    ESP_LOGI(TAG, "ready for next wakeword");
}

static void export_recording(void) {
    exporting = true;
    wake_afe_pause(); // stop audio processing during export
    set_recording_led(false);

    uint8_t *wav = NULL;
    size_t wav_len = 0;
    ESP_ERROR_CHECK(wav_from_pcm16_mono( // convert PCM to WAV
        recorder_samples(),
        recorder_sample_count(),
        BOARD_AUDIO_SAMPLE_RATE,
        &wav,
        &wav_len));

    ESP_LOGI(TAG, "recording finished duration_ms=%u sample_count=%u wav_bytes=%u",
             (unsigned)recorder_duration_ms(),
             (unsigned)recorder_sample_count(),
             (unsigned)wav_len);
    esp_err_t ret = capture_output_send_wav(wav, wav_len); // send to output
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "capture output failed err=0x%x", (unsigned)ret);
        esp_err_t beep_ret = board_audio_play_error_beep();
        if (beep_ret != ESP_OK) {
            ESP_LOGE(TAG, "error beep failed err=0x%x", (unsigned)beep_ret);
        }
    }
    free(wav);
    reset_for_next_turn();
}

// Called by AFE fetch task for every processed mono frame. Feed to recorder, trigger on wakeword, export when finished.
//
// Args: samples - processed audio samples
//       sample_count - number of samples
//       wake_state - wakeword detection state
// Returns: void
void voice_session_on_audio(const int16_t *samples, int sample_count, int wake_state) {
    if (exporting) return;

    recorder_push(samples, sample_count); // feed to recorder

    if (!triggered && wake_state == WAKENET_DETECTED) {
        triggered = true;
        recorder_trigger(); // start recording with pre-roll
        set_recording_led(true);
        ESP_LOGI(TAG, "wakeword detected; recording started with pre-roll");
    }

    if (recorder_state() == RECORDER_FINISHED) {
        export_recording(); // silence stop or max duration reached
    }
}
