// Minimal WAV writer. PCM16 mono only.
#include "wav.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

// WAV fields are little-endian.
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = v >> 24;
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t wav_from_pcm16_mono(const int16_t *samples, size_t sample_count, uint32_t sample_rate, uint8_t **out, size_t *out_len) {
    size_t data_len = sample_count * sizeof(int16_t);
    size_t wav_len = 44 + data_len;
    uint8_t *wav = heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) return ESP_ERR_NO_MEM;

    memcpy(wav + 0, "RIFF", 4);
    put_u32(wav + 4, 36 + data_len);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    put_u32(wav + 16, 16);
    put_u16(wav + 20, 1);
    put_u16(wav + 22, 1);
    put_u32(wav + 24, sample_rate);
    put_u32(wav + 28, sample_rate * 2);
    put_u16(wav + 32, 2);
    put_u16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    put_u32(wav + 40, data_len);
    memcpy(wav + 44, samples, data_len);

    *out = wav;
    *out_len = wav_len;
    return ESP_OK;
}

esp_err_t wav_parse_pcm16_mono(const uint8_t *wav, size_t wav_len, wav_pcm16_view_t *out) {
    if (!wav || !out) return ESP_ERR_INVALID_ARG;
    if (wav_len < 12) return ESP_ERR_INVALID_SIZE;
    if (memcmp(wav + 0, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool found_fmt = false;
    bool found_data = false;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0;

    size_t pos = 12;
    while (pos + 8 <= wav_len) {
        const uint8_t *chunk = wav + pos;
        uint32_t chunk_len = read_u32(chunk + 4);
        size_t chunk_data_pos = pos + 8;
        if (chunk_data_pos + chunk_len > wav_len) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_len < 16) return ESP_ERR_INVALID_SIZE;
            audio_format = read_u16(wav + chunk_data_pos + 0);
            channels = read_u16(wav + chunk_data_pos + 2);
            sample_rate = read_u32(wav + chunk_data_pos + 4);
            bits_per_sample = read_u16(wav + chunk_data_pos + 14);
            found_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data = wav + chunk_data_pos;
            data_len = chunk_len;
            found_data = true;
        }

        pos = chunk_data_pos + chunk_len + (chunk_len & 1u);
    }

    if (!found_fmt || !found_data) return ESP_ERR_NOT_FOUND;
    if (audio_format != 1 || channels != 1 || bits_per_sample != 16) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((data_len % sizeof(int16_t)) != 0) return ESP_ERR_INVALID_SIZE;

    out->samples = (const int16_t *)data;
    out->sample_count = data_len / sizeof(int16_t);
    out->sample_rate_hz = sample_rate;
    out->channels = channels;
    out->bits_per_sample = bits_per_sample;
    return ESP_OK;
}
