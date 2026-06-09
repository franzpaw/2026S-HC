#pragma once

// PCM16 mono -> WAV bytes.

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const int16_t *samples;
    size_t sample_count;
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
} wav_pcm16_view_t;

// Allocates output in PSRAM.
esp_err_t wav_from_pcm16_mono(const int16_t *samples, size_t sample_count, uint32_t sample_rate, uint8_t **out, size_t *out_len);

// Finds PCM16 mono samples inside a WAV buffer. Does not copy data.
esp_err_t wav_parse_pcm16_mono(const uint8_t *wav, size_t wav_len, wav_pcm16_view_t *out);
