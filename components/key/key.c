#include "key.h"
#include <stdbool.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "key";

/* ================= 消抖参数 ================= */
#define DEBOUNCE_MS       30    /* 消抖等待时间 */
#define QUEUE_LEN         8     /* 事件队列长度 */

/* 队列中传递的信息：仅用 int 表示边沿类型（0=下降沿, 1=上升沿） */
static QueueHandle_t     s_evt_queue   = NULL;
static TaskHandle_t      s_debounce_task = NULL;
static key_event_cb_t    s_user_cb     = NULL;
static void             *s_user_ctx    = NULL;
static bool              s_last_state  = false;  /* 上次确认后的按键状态 */

/* ================= GPIO ISR ================= */
static void IRAM_ATTR key_isr_handler(void *arg)
{
    /*
     * ISR 中只做最小工作：将当前 GPIO 电平（即边沿方向）推入队列。
     * 0 = LOW（下降沿，按下）
     * 1 = HIGH（上升沿，松开）
     */
    int level = gpio_get_level(KEY_GPIO);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_evt_queue, &level, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* ================= 消抖任务 ================= */
static void debounce_task(void *arg)
{
    int level;
    ESP_LOGI(TAG, "消抖任务启动");

    while (true) {
        /* 阻塞等待 ISR 推入的边沿事件 */
        if (xQueueReceive(s_evt_queue, &level, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * 收到边沿事件后，等待 DEBOUNCE_MS 让机械触点稳定，
         * 然后读取 GPIO 实际电平，作为最终确认状态。
         */
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        bool confirmed = (gpio_get_level(KEY_GPIO) == 0);  /* LOW = 按下 */

        /* 状态无变化则忽略（抖动被滤除） */
        if (confirmed == s_last_state) {
            continue;
        }

        s_last_state = confirmed;

        key_event_t event = confirmed ? KEY_EVENT_PRESSED : KEY_EVENT_RELEASED;
        ESP_LOGI(TAG, "按键%s", confirmed ? "按下" : "松开");

        /* 在任务上下文调用用户回调（可安全调用阻塞 API） */
        if (s_user_cb) {
            s_user_cb(event, s_user_ctx);
        }

        /*
         * 消抖期间可能还有新的边沿事件积压在队列中，
         * 它们描述的是同一次物理动作，全部丢弃。
         */
        while (xQueueReceive(s_evt_queue, &level, 0) == pdTRUE) {
            /* 清空队列 */
        }
    }
}

/* ================= 公共接口 ================= */

void key_init(void)
{
    /* 配置 GPIO：输入，双边沿中断 */
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_NEGEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << KEY_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);

    /* 读取初始状态 */
    s_last_state = (gpio_get_level(KEY_GPIO) == 0);

    /* 创建事件队列 */
    if (s_evt_queue == NULL) {
        s_evt_queue = xQueueCreate(QUEUE_LEN, sizeof(int));
    }

    /* 安装 GPIO ISR 服务（仅首次）并注册处理函数 */
    static bool isr_service_installed = false;
    if (!isr_service_installed) {
        gpio_install_isr_service(0);
        isr_service_installed = true;
    }
    gpio_isr_handler_add(KEY_GPIO, key_isr_handler, NULL);

    /* 创建消抖任务 */
    if (s_debounce_task == NULL) {
        xTaskCreate(debounce_task, "key_debounce", 2048, NULL, 6, &s_debounce_task);
    }

    ESP_LOGI(TAG, "按键初始化完成, GPIO=%d, 初始状态=%s",
             KEY_GPIO, s_last_state ? "按下" : "松开");
}

void key_set_callback(key_event_cb_t cb, void *user_ctx)
{
    s_user_cb  = cb;
    s_user_ctx = user_ctx;
}

bool key_is_pressed(void)
{
    return (gpio_get_level(KEY_GPIO) == 0);
}

uint8_t key_scan(uint8_t mode)
{
    uint8_t keyvalue = 0;
    static uint8_t key_boot = 1;
    if (mode) {
        key_boot = 1;
    }

    if (key_boot && (gpio_get_level(KEY_GPIO) == 0)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        key_boot = 0;
        if (gpio_get_level(KEY_GPIO) == 0) {
            keyvalue = 1;
        }
    } else if (gpio_get_level(KEY_GPIO) == 1) {
        key_boot = 1;
    }
    return keyvalue;
}
