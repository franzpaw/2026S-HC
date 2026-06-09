#pragma once

// PCM recorder with pre-roll, silence stop, hard max.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    RECORDER_IDLE = 0,
    RECORDER_RECORDING,
    RECORDER_FINISHED,
} recorder_state_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t pre_roll_ms;
    uint32_t silence_stop_ms;
    uint32_t max_recording_ms;
    uint16_t silence_threshold;
} recorder_config_t;

// Allocate fixed max-duration buffers.
esp_err_t recorder_init(recorder_config_t config);

// Add PCM; stores pre-roll or active recording.
void recorder_push(const int16_t *samples, size_t count);

// Start recording and prepend pre-roll.
void recorder_trigger(void);

// Return to idle after one completed turn.
void recorder_reset(void);

recorder_state_t recorder_state(void);
const int16_t *recorder_samples(void);
size_t recorder_sample_count(void);
uint32_t recorder_duration_ms(void);
