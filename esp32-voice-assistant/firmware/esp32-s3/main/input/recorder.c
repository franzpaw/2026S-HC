// Simple fixed-buffer recorder. No realloc during capture.
#include "recorder.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

static recorder_config_t cfg;
static recorder_state_t state;
static int16_t *pre_roll; // ring buffer
static size_t pre_roll_cap;
static size_t pre_roll_len;
static size_t pre_roll_pos; // write position
static int16_t *samples; // main buffer
static size_t samples_len;
static size_t samples_cap;
static size_t recorded_after_trigger;
static size_t silent_samples;

// Convert wall time to PCM sample count.
//
// Args: ms - milliseconds
// Returns: sample count
static size_t samples_for_ms(uint32_t ms) {
    return ((uint64_t)cfg.sample_rate_hz * ms) / 1000;
}

// Init recorder: allocate buffers for pre-roll and max recording.
//
// Args: config - recorder configuration
// Returns: ESP_OK on success, ESP_ERR_NO_MEM otherwise
esp_err_t recorder_init(recorder_config_t config) {
    cfg = config;
    state = RECORDER_IDLE;
    pre_roll_cap = samples_for_ms(cfg.pre_roll_ms);
    samples_cap = samples_for_ms(cfg.pre_roll_ms + cfg.max_recording_ms);

    pre_roll = heap_caps_calloc(pre_roll_cap, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // PSRAM
    samples = heap_caps_calloc(samples_cap, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // PSRAM
    return pre_roll && samples ? ESP_OK : ESP_ERR_NO_MEM;
}

// Ring buffer before wakeword - keeps last 1.2s.
//
// Args: sample - audio sample
// Returns: void
static void push_pre_roll(int16_t sample) {
    if (pre_roll_cap == 0) return;
    pre_roll[pre_roll_pos] = sample;
    pre_roll_pos = (pre_roll_pos + 1) % pre_roll_cap; // wrap around
    if (pre_roll_len < pre_roll_cap) pre_roll_len++;
}

// Append active sample, track silence, stop on silence or max duration.
//
// Args: sample - audio sample
// Returns: void
static void append_sample(int16_t sample) {
    if (samples_len >= samples_cap) {
        state = RECORDER_FINISHED;
        return;
    }
    samples[samples_len++] = sample;
    recorded_after_trigger++;

    uint16_t amp = sample == INT16_MIN ? 32768u : (uint16_t)(sample < 0 ? -sample : sample); // absolute amplitude
    if (amp <= cfg.silence_threshold) {
        silent_samples++; // count silence
    } else {
        silent_samples = 0; // reset on sound
    }

    if (silent_samples >= samples_for_ms(cfg.silence_stop_ms) || // 2.5s silence
        recorded_after_trigger >= samples_for_ms(cfg.max_recording_ms)) { // 10s max
        state = RECORDER_FINISHED;
    }
}

// Push samples: pre-roll before trigger, active recording after.
//
// Args: input - audio samples
//       count - number of samples
// Returns: void
void recorder_push(const int16_t *input, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (state == RECORDER_IDLE) {
            push_pre_roll(input[i]);
        } else if (state == RECORDER_RECORDING) {
            append_sample(input[i]);
        }
    }
}

// Trigger recording: copy pre-roll to main buffer, start active recording.
//
// Args: none
// Returns: void
void recorder_trigger(void) {
    if (state != RECORDER_IDLE) return;

    size_t start = pre_roll_len == pre_roll_cap ? pre_roll_pos : 0; // oldest sample first
    for (size_t i = 0; i < pre_roll_len; i++) {
        samples[samples_len++] = pre_roll[(start + i) % pre_roll_cap]; // sequential read
    }
    state = RECORDER_RECORDING;
}

void recorder_reset(void) {
    state = RECORDER_IDLE;
    pre_roll_len = 0;
    pre_roll_pos = 0;
    samples_len = 0;
    recorded_after_trigger = 0;
    silent_samples = 0;
}

// Get recorder state.
//
// Args: none
// Returns: current recorder state
recorder_state_t recorder_state(void) { return state; }

// Get recorded samples.
//
// Args: none
// Returns: pointer to sample buffer
const int16_t *recorder_samples(void) { return samples; }

// Get sample count.
//
// Args: none
// Returns: number of samples
size_t recorder_sample_count(void) { return samples_len; }

// Get recording duration in milliseconds.
//
// Args: none
// Returns: duration in ms
uint32_t recorder_duration_ms(void) {
    return ((uint64_t)samples_len * 1000) / cfg.sample_rate_hz;
}
