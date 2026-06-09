#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Calls GET /health to verify DNS, TLS, auth, and backend reachability.
esp_err_t backend_client_health_check(void);

typedef void (*backend_audio_callback_t)(const uint8_t *audio_data, size_t audio_size, const char *format, void *user_ctx);

// Uploads one WAV recording to /chat/stream and reports final response audio bytes.
esp_err_t backend_client_chat_audio(const uint8_t *wav_data,
                                    size_t wav_size,
                                    backend_audio_callback_t audio_callback,
                                    void *user_ctx);
