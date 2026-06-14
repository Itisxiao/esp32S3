#ifndef INMP441_H
#define INMP441_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 默认 GPIO 引脚定义 ================= */
#define INMP441_BCK_GPIO    GPIO_NUM_4    /* I2S 位时钟 (BCLK) */
#define INMP441_WS_GPIO     GPIO_NUM_5    /* I2S 字选择 (LRCLK/WS) */
#define INMP441_DIN_GPIO    GPIO_NUM_6    /* I2S 数据输入 (SD/DOUT from mic) */

/* ================= 默认音频参数 ================= */
#define INMP441_DEFAULT_SAMPLE_RATE     16000   /* 采样率 16kHz */
#define INMP441_DEFAULT_BITS_PER_SAMPLE 32      /* INMP441 输出 32bit 数据 */

/* ================= 配置结构体 ================= */
typedef struct {
    gpio_num_t bck_gpio;      /* BCLK 引脚 */
    gpio_num_t ws_gpio;       /* WS/LRCLK 引脚 */
    gpio_num_t din_gpio;      /* SD 数据输出引脚 (mic -> ESP) */
    uint32_t   sample_rate;   /* 采样率, 默认 16000 */
} inmp441_config_t;

/* ================= API ================= */

/**
 * @brief  使用默认配置初始化 INMP441 麦克风
 * @return ESP_OK 成功
 */
esp_err_t inmp441_init(void);

/**
 * @brief  使用自定义配置初始化 INMP441 麦克风
 * @param  config 配置参数, 为 NULL 时使用默认值
 * @return ESP_OK 成功
 */
esp_err_t inmp441_init_with_config(const inmp441_config_t *config);

/**
 * @brief  反初始化, 释放 I2S 资源
 * @return ESP_OK 成功
 */
esp_err_t inmp441_deinit(void);

/**
 * @brief  从麦克风读取音频数据 (阻塞)
 * @param  samples       输出缓冲区 (int16_t 数组)
 * @param  frame_count   要读取的采样帧数
 * @param  timeout_ms    超时毫秒数
 * @param  frames_read   实际读取的帧数 (可为 NULL)
 * @return ESP_OK 成功
 *
 * @note   内部 32bit 数据会自动右移转为 16bit 输出
 */
esp_err_t inmp441_read(int16_t *samples, size_t frame_count,
                       uint32_t timeout_ms, size_t *frames_read);

/**
 * @brief  从麦克风读取原始 32bit 数据 (阻塞)
 * @param  samples       输出缓冲区 (int32_t 数组)
 * @param  frame_count   要读取的采样帧数
 * @param  timeout_ms    超时毫秒数
 * @param  frames_read   实际读取的帧数 (可为 NULL)
 * @return ESP_OK 成功
 */
esp_err_t inmp441_read_raw(int32_t *samples, size_t frame_count,
                           uint32_t timeout_ms, size_t *frames_read);

/**
 * @brief  检测是否有有效的音频输入 (音量超过阈值)
 * @param  threshold  音量阈值 (0~32767), 推荐 500~2000
 * @param  has_voice  输出: true 表示检测到语音
 * @param  volume_out 输出: 当前音量值 (可为 NULL)
 * @return ESP_OK 成功
 */
esp_err_t inmp441_detect_voice(int threshold, bool *has_voice, int *volume_out);

/**
 * @brief  获取当前麦克风的音量 (RMS)
 * @param  volume  输出音量值 (0~32767)
 * @return ESP_OK 成功
 */
esp_err_t inmp441_get_volume(int *volume);

#ifdef __cplusplus
}
#endif

#endif /* INMP441_H */
