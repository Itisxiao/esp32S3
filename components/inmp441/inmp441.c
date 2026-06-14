#include "inmp441.h"

#include <string.h>
#include <math.h>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "inmp441";
static i2s_chan_handle_t s_rx_handle = NULL;
static uint32_t s_sample_rate = INMP441_DEFAULT_SAMPLE_RATE;

/* 内部读取缓冲区 (32bit), 用于转换 */
#define INTERNAL_BUF_SIZE 512
static int32_t s_raw_buf[INTERNAL_BUF_SIZE];

esp_err_t inmp441_init(void)
{
    const inmp441_config_t config = {
        .bck_gpio   = INMP441_BCK_GPIO,
        .ws_gpio    = INMP441_WS_GPIO,
        .din_gpio   = INMP441_DIN_GPIO,
        .sample_rate = INMP441_DEFAULT_SAMPLE_RATE,
    };
    return inmp441_init_with_config(&config);
}

esp_err_t inmp441_init_with_config(const inmp441_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    /* 已初始化则直接返回 */
    if (s_rx_handle != NULL) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    s_sample_rate = config->sample_rate == 0 ? INMP441_DEFAULT_SAMPLE_RATE : config->sample_rate;

    ESP_LOGI(TAG, "Init INMP441: BCK=%d, WS=%d, DIN=%d, rate=%lu",
             config->bck_gpio, config->ws_gpio, config->din_gpio,
             (unsigned long)s_sample_rate);

    /* ---- 创建 I2S RX 通道 ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_rx_handle = NULL;
        return err;
    }

    /* ---- 配置 I2S 标准模式 (Philips 格式) ---- */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bck_gpio,
            .ws   = config->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din  = config->din_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* L/R 接 GND => 左声道有效; 飞利浦格式中 WS 低电平 = 左声道 */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return err;
    }

    err = i2s_channel_enable(s_rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return err;
    }

    /* 丢弃前几帧数据, 等待 I2S 稳定 */
    int32_t discard_buf[64];
    size_t bytes_read = 0;
    for (int i = 0; i < 3; i++) {
        i2s_channel_read(s_rx_handle, discard_buf, sizeof(discard_buf),
                         &bytes_read, pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "INMP441 init OK, sample rate=%lu", (unsigned long)s_sample_rate);
    return ESP_OK;
}

esp_err_t inmp441_deinit(void)
{
    if (s_rx_handle == NULL) {
        return ESP_OK;
    }
    i2s_channel_disable(s_rx_handle);
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
    ESP_LOGI(TAG, "INMP441 deinit OK");
    return ESP_OK;
}

esp_err_t inmp441_read(int16_t *samples, size_t frame_count,
                       uint32_t timeout_ms, size_t *frames_read)
{
    ESP_RETURN_ON_FALSE(s_rx_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "samples is NULL");

    size_t total_read = 0;
    size_t remaining  = frame_count;

    while (remaining > 0) {
        size_t chunk = remaining > INTERNAL_BUF_SIZE ? INTERNAL_BUF_SIZE : remaining;
        size_t bytes_read = 0;

        esp_err_t err = i2s_channel_read(s_rx_handle, s_raw_buf,
                                         chunk * sizeof(int32_t),
                                         &bytes_read,
                                         pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
            if (frames_read) *frames_read = total_read;
            return err;
        }

        size_t frames_this_read = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < frames_this_read; i++) {
            /* INMP441 数据在高 24 位, 右移 14 位转为 16bit 有效数据 */
            int32_t raw = s_raw_buf[i];
            samples[total_read + i] = (int16_t)(raw >> 14);
        }

        total_read += frames_this_read;
        remaining  -= frames_this_read;

        if (frames_this_read < chunk) {
            /* 读取不完整, 提前退出 */
            break;
        }
    }

    if (frames_read) *frames_read = total_read;
    return ESP_OK;
}

esp_err_t inmp441_read_raw(int32_t *samples, size_t frame_count,
                           uint32_t timeout_ms, size_t *frames_read)
{
    ESP_RETURN_ON_FALSE(s_rx_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "samples is NULL");

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_handle, samples,
                                     frame_count * sizeof(int32_t),
                                     &bytes_read,
                                     pdMS_TO_TICKS(timeout_ms));
    if (frames_read) *frames_read = bytes_read / sizeof(int32_t);
    return err;
}

/**
 * @brief  计算 RMS 音量 (使用 int16 采样值)
 */
static int calc_rms_volume(const int16_t *samples, size_t count)
{
    if (count == 0) return 0;
    int64_t sum_sq = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        sum_sq += s * s;
    }
    return (int)sqrtf((float)(sum_sq / count));
}

esp_err_t inmp441_get_volume(int *volume)
{
    ESP_RETURN_ON_FALSE(volume != NULL, ESP_ERR_INVALID_ARG, TAG, "volume is NULL");

    int16_t buf[256];
    size_t frames_read = 0;
    esp_err_t err = inmp441_read(buf, 256, 200, &frames_read);
    if (err != ESP_OK) return err;

    *volume = calc_rms_volume(buf, frames_read);
    return ESP_OK;
}

esp_err_t inmp441_detect_voice(int threshold, bool *has_voice, int *volume_out)
{
    ESP_RETURN_ON_FALSE(has_voice != NULL, ESP_ERR_INVALID_ARG, TAG, "has_voice is NULL");

    int vol = 0;
    esp_err_t err = inmp441_get_volume(&vol);
    if (err != ESP_OK) return err;

    *has_voice = (vol > threshold);
    if (volume_out) *volume_out = vol;
    return ESP_OK;
}
