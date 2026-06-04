#ifndef MAX98357A_H
#define MAX98357A_H

#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX98357A_DIN_GPIO  GPIO_NUM_7
#define MAX98357A_BCLK_GPIO GPIO_NUM_15
#define MAX98357A_LRC_GPIO  GPIO_NUM_16

#define MAX98357A_DEFAULT_SAMPLE_RATE 16000

typedef struct {
    gpio_num_t din_gpio;
    gpio_num_t bclk_gpio;
    gpio_num_t lrc_gpio;
    uint32_t sample_rate;
} max98357a_config_t;

esp_err_t max98357a_init(void);
esp_err_t max98357a_init_with_config(const max98357a_config_t *config);
esp_err_t max98357a_write(const int16_t *samples, size_t frame_count, uint32_t timeout_ms);
esp_err_t max98357a_play_tone(uint32_t frequency_hz, uint32_t duration_ms, int16_t volume);
esp_err_t max98357a_deinit(void);
esp_err_t max98357a_play_wav(const uint8_t *wav_data, size_t wav_size);

#ifdef __cplusplus
}
#endif

#endif
