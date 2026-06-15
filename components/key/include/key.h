#ifndef __KEY_H__
#define __KEY_H__

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KEY_GPIO GPIO_NUM_0

/* ================= 按键事件 ================= */
typedef enum {
    KEY_EVENT_PRESSED,    /* 按下（下降沿确认） */
    KEY_EVENT_RELEASED,   /* 松开（上升沿确认） */
} key_event_t;

/**
 * @brief  按键事件回调（在消抖任务上下文调用，非 ISR）
 * @param  event    按下或松开
 * @param  user_ctx 用户自定义指针
 */
typedef void (*key_event_cb_t)(key_event_t event, void *user_ctx);

/* ================= API ================= */

/**
 * @brief  初始化按键 GPIO 及中断服务
 *         调用后中断已使能，但需通过 key_set_callback 注册回调
 */
void key_init(void);

/**
 * @brief  注册按键事件回调（取代轮询）
 * @param  cb       回调函数（在消抖任务中调用，可调用阻塞 API）
 * @param  user_ctx 透传给回调的用户指针（可为 NULL）
 */
void key_set_callback(key_event_cb_t cb, void *user_ctx);

/**
 * @brief  查询按键是否正在按下（实时 GPIO 电平）
 * @return true=按键按下, false=按键松开
 */
bool key_is_pressed(void);

/**
 * @brief  旧版单次扫描接口（保持向后兼容）
 */
uint8_t key_scan(uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif /* __KEY_H__ */
