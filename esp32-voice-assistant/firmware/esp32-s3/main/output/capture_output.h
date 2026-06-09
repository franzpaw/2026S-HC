#pragma once

// Finished capture output seam. Serial today; backend later.

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Send one complete WAV buffer.
esp_err_t capture_output_send_wav(const uint8_t *wav, size_t wav_len);
