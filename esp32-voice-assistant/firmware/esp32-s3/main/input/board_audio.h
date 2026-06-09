#pragma once

// Waveshare ESP32-S3 Audio Board mic input adapter.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BOARD_AUDIO_SAMPLE_RATE 16000
#define BOARD_AUDIO_FEED_CHANNELS 4

// Init ES7210 + I2S1 for 16 kHz capture.
esp_err_t board_audio_init(void);

// Read interleaved AFE feed PCM.
esp_err_t board_audio_read_feed(int16_t *buffer, int byte_len);

// Play 16 kHz mono PCM through the board speaker.
esp_err_t board_audio_write_pcm16_mono(const int16_t *samples, size_t sample_count, uint32_t sample_rate_hz);

// Signal a failed turn to the user.
esp_err_t board_audio_play_error_beep(void);

// AFE feed channel count.
int board_audio_feed_channels(void);

// ESP-SR AFE channel layout.
const char *board_audio_input_format(void);
