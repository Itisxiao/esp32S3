#include "max98357a.h"

#include <string.h>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "max98357a";
static i2s_chan_handle_t s_tx_handle = NULL;
static uint32_t s_sample_rate = MAX98357A_DEFAULT_SAMPLE_RATE;

esp_err_t max98357a_init(void)
{
    const max98357a_config_t config = {
        .din_gpio = MAX98357A_DIN_GPIO,
        .bclk_gpio = MAX98357A_BCLK_GPIO,
        .lrc_gpio = MAX98357A_LRC_GPIO,
        .sample_rate = MAX98357A_DEFAULT_SAMPLE_RATE,
    };

    return max98357a_init_with_config(&config);
}

esp_err_t max98357a_init_with_config(const max98357a_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    if (s_tx_handle != NULL) {
        return ESP_OK;
    }

    s_sample_rate = config->sample_rate == 0 ? MAX98357A_DEFAULT_SAMPLE_RATE : config->sample_rate;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL), TAG, "create tx channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_gpio,
            .ws = config->lrc_gpio,
            .dout = config->din_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    esp_err_t err = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "init std mode failed");
    }

    err = i2s_channel_enable(s_tx_handle);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "enable tx channel failed");
    }

    return ESP_OK;
}

esp_err_t max98357a_write(const int16_t *samples, size_t frame_count, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_tx_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "samples is NULL");

    size_t bytes_written = 0;
    const size_t bytes_to_write = frame_count * 2 * sizeof(int16_t);
    return i2s_channel_write(s_tx_handle, samples, bytes_to_write, &bytes_written, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t max98357a_play_tone(uint32_t frequency_hz, uint32_t duration_ms, int16_t volume)
{
    ESP_RETURN_ON_FALSE(s_tx_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(frequency_hz > 0 && duration_ms > 0, ESP_ERR_INVALID_ARG, TAG, "invalid tone");

    int16_t buffer[256 * 2];
    const uint32_t total_frames = (s_sample_rate * duration_ms) / 1000;
    uint32_t phase = 0;
    uint32_t frames_written = 0;

    while (frames_written < total_frames) {
        const uint32_t chunk_frames = (total_frames - frames_written) > 256 ? 256 : (total_frames - frames_written);

        for (uint32_t i = 0; i < chunk_frames; i++) {
            const int16_t sample = phase < (s_sample_rate / 2) ? volume : -volume;
            buffer[i * 2] = sample;
            buffer[i * 2 + 1] = sample;

            phase += frequency_hz;
            while (phase >= s_sample_rate) {
                phase -= s_sample_rate;
            }
        }

        esp_err_t err = max98357a_write(buffer, chunk_frames, 1000);
        if (err != ESP_OK) {
            return err;
        }
        frames_written += chunk_frames;
    }

    memset(buffer, 0, sizeof(buffer));
    return max98357a_write(buffer, 256, 1000);
}

esp_err_t max98357a_deinit(void)
{
    if (s_tx_handle == NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_handle), TAG, "disable tx channel failed");
    ESP_RETURN_ON_ERROR(i2s_del_channel(s_tx_handle), TAG, "delete tx channel failed");
    s_tx_handle = NULL;
    return ESP_OK;
}
