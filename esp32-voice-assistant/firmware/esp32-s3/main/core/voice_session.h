#pragma once

// One spoken turn: wakeword -> recording -> WAV -> output.

#include <stdint.h>
#include "esp_err.h"

// Allocate recorder state for one capture.
esp_err_t voice_session_init(void);

// Consume AFE mono audio and wake state.
void voice_session_on_audio(const int16_t *samples, int sample_count, int wake_state);
