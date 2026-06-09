#pragma once

// ESP-SR AFE adapter. Hides model, feed/fetch tasks, wake states.

#include <stdint.h>
#include "esp_err.h"

// Processed mono audio plus normalized wake state.
typedef void (*wake_afe_audio_cb_t)(const int16_t *samples, int sample_count, int wake_state);

// Start AFE feed/fetch tasks.
esp_err_t wake_afe_start(wake_afe_audio_cb_t callback);

// Stop AFE before long output work.
void wake_afe_pause(void);

// Continue listening after output work.
void wake_afe_resume(void);
