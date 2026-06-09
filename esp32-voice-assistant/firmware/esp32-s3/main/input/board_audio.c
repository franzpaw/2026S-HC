// Hardware adapter copied down from Waveshare demo essentials only.
#include "board_audio.h"

#include <stdlib.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_io_expander.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "es7210_adc.h"
#include "es8311_codec.h"

#define I2C_PORT I2C_NUM_0
#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define I2S_PORT I2S_NUM_1
#define I2S_MCLK GPIO_NUM_12
#define I2S_BCLK GPIO_NUM_13
#define I2S_WS GPIO_NUM_14
#define I2S_DIN GPIO_NUM_15
#define I2S_DOUT GPIO_NUM_16
#define RECORD_GAIN 30.0f

static const char *TAG = "board_audio";

static i2c_master_bus_handle_t i2c_bus;
static i2s_chan_handle_t tx_handle;
static i2s_chan_handle_t rx_handle;
static esp_codec_dev_handle_t record_dev;
static esp_codec_dev_handle_t play_dev;
static esp_io_expander_handle_t io_expander;

// Control bus for ES7210 codec.
static esp_err_t init_i2c(void) {
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    return i2c_new_master_bus(&cfg, &i2c_bus);
}

// Audio bus: ES7210 mic samples into ESP32-S3.
static esp_err_t init_i2s(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG, "tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_handle, &std_cfg), TAG, "rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle), TAG, "tx enable failed");
    return i2s_channel_enable(rx_handle);
}

// Match Waveshare board expander setup for external board control pins.
static esp_err_t init_tca9555(void) {
    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &io_expander),
        TAG,
        "tca9555 init failed");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(io_expander,
                                IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_5 |
                                    IO_EXPANDER_PIN_NUM_6 | IO_EXPANDER_PIN_NUM_8,
                                IO_EXPANDER_OUTPUT),
        TAG,
        "tca9555 output dir failed");
    return esp_io_expander_set_dir(io_expander,
                                   IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_9 | IO_EXPANDER_PIN_NUM_10 |
                                       IO_EXPANDER_PIN_NUM_11,
                                   IO_EXPANDER_INPUT);
}

// Enable the board speaker DAC.
static esp_err_t init_es8311(void) {
    audio_codec_i2s_cfg_t i2s_cfg = {.rx_handle = NULL, .tx_handle = tx_handle};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) return ESP_FAIL;

    audio_codec_i2c_cfg_t i2c_cfg = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) return ESP_FAIL;

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!gpio_if) return ESP_FAIL;

    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = -1,
        .use_mclk = false,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (!codec_if) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    play_dev = esp_codec_dev_new(&dev_cfg);
    if (!play_dev) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BOARD_AUDIO_SAMPLE_RATE,
        .channel = 2,
        .bits_per_sample = 32,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev, CONFIG_VOICE_PLAY_VOLUME), TAG, "set volume failed");
    return esp_codec_dev_open(play_dev, &fs);
}

// Enable all four board microphones.
static esp_err_t init_es7210(void) {
    audio_codec_i2s_cfg_t i2s_cfg = {.rx_handle = rx_handle, .tx_handle = NULL};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) return ESP_FAIL;

    audio_codec_i2c_cfg_t i2c_cfg = {.addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) return ESP_FAIL;

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&es7210_cfg);
    if (!codec_if) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    record_dev = esp_codec_dev_new(&dev_cfg);
    if (!record_dev) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BOARD_AUDIO_SAMPLE_RATE,
        .channel = 2,
        .bits_per_sample = 32,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(record_dev, &fs), TAG, "codec open failed");

    for (int ch = 0; ch < BOARD_AUDIO_FEED_CHANNELS; ch++) {
        esp_codec_dev_set_in_channel_gain(record_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(ch), RECORD_GAIN);
    }
    return ESP_OK;
}

esp_err_t board_audio_init(void) {
    if (record_dev) return ESP_OK;
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "i2c init failed");
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "i2s init failed");
    ESP_RETURN_ON_ERROR(init_tca9555(), TAG, "tca9555 init failed");
    ESP_RETURN_ON_ERROR(init_es7210(), TAG, "es7210 init failed");
    ESP_RETURN_ON_ERROR(init_es8311(), TAG, "es8311 init failed");
    ESP_LOGI(TAG, "ready: ES7210 4-channel feed + ES8311 speaker, 16 kHz");
    return ESP_OK;
}

esp_err_t board_audio_read_feed(int16_t *buffer, int byte_len) {
    return esp_codec_dev_read(record_dev, buffer, byte_len);
}

esp_err_t board_audio_play_error_beep(void) {
    int16_t tone[128];
    int16_t silence[128] = {0};

    for (size_t i = 0; i < sizeof(tone) / sizeof(tone[0]); i++) {
        tone[i] = ((i / 16) % 2 == 0) ? 3500 : -3500;
    }

    ESP_RETURN_ON_ERROR(board_audio_write_pcm16_mono(tone, sizeof(tone) / sizeof(tone[0]), BOARD_AUDIO_SAMPLE_RATE), TAG, "error beep first tone");
    ESP_RETURN_ON_ERROR(board_audio_write_pcm16_mono(silence, sizeof(silence) / sizeof(silence[0]), BOARD_AUDIO_SAMPLE_RATE), TAG, "error beep gap");
    return board_audio_write_pcm16_mono(tone, sizeof(tone) / sizeof(tone[0]), BOARD_AUDIO_SAMPLE_RATE);
}

esp_err_t board_audio_write_pcm16_mono(const int16_t *samples, size_t sample_count, uint32_t sample_rate_hz) {
    if (!play_dev || !samples || sample_rate_hz != BOARD_AUDIO_SAMPLE_RATE) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t chunk_frames = 512;
    int32_t out[chunk_frames * 2];
    size_t written = 0;
    while (written < sample_count) {
        size_t frames = sample_count - written;
        if (frames > chunk_frames) {
            frames = chunk_frames;
        }

        for (size_t i = 0; i < frames; i++) {
            int32_t sample = ((int32_t)samples[written + i]) << 16;
            out[i * 2] = sample;
            out[i * 2 + 1] = sample;
        }

        ESP_RETURN_ON_ERROR(esp_codec_dev_write(play_dev, out, frames * 2 * sizeof(out[0])), TAG, "speaker write failed");
        written += frames;
    }

    int32_t silence[64] = {0};
    for (int i = 0; i < 25; i++) {
        ESP_RETURN_ON_ERROR(esp_codec_dev_write(play_dev, silence, sizeof(silence)), TAG, "speaker silence failed");
    }

    return ESP_OK;
}

int board_audio_feed_channels(void) {
    return BOARD_AUDIO_FEED_CHANNELS;
}

// Demo format: Reference, Mic, Unused, Mic.
const char *board_audio_input_format(void) {
    return "RMNM";
}
