#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lcd_st7789.h"
#include "max98357a.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "websocket_client.h"
#include "enter_config.h"
#include "inmp441.h"
#include "key.h"
#include "vad.h"
#include <cstring>
#include <cstdio>
#include <cmath>

static const char *TAG = "main";

// TODO: 替换为你的后端服务器地址
#define WS_SERVER_URI "ws://192.168.0.100:8080//ws/audio"

// ================= EventGroup 事件位定义 =================
#define EVENT_NETWORK_CONNECTED   BIT0   // 获取到 IP
#define EVENT_NETWORK_DISCONNECTED BIT1   // WiFi 断开
#define EVENT_WS_CONNECTED        BIT2   // WebSocket 已连接
#define EVENT_WS_DISCONNECTED     BIT3   // WebSocket 断开
#define EVENT_TIMEOUT_TICK        BIT4   // 1秒定时心跳
#define EVENT_VAD_START           BIT5   // VAD 检测到语音
#define EVENT_VAD_STOP            BIT6   // VAD 确认静音

// ================= 录音参数 =================
#define RECORD_CHUNK_MS       20    // 每次发送 20ms 音频
#define AUDIO_QUEUE_LEN      30    // 音频队列深度 (30 × 20ms = 600ms 缓冲)
// 16kHz × 20ms = 320 frames × 2 bytes = 640 bytes per chunk
#define AUDIO_CHUNK_BYTES    640

// ================= 全局状态 =================
static EventGroupHandle_t s_event_group = NULL;
static bool s_lcd_ok = false;
static bool s_ws_started = false;

// ================= 音频队列：解耦 I2S 读取和 WebSocket 发送 =================
static QueueHandle_t s_audio_queue = NULL;
static TaskHandle_t  s_audio_sender_task = NULL;
static volatile bool s_sender_running = false;

// VAD / 手动录音模式标志
static volatile bool s_vad_recording = false;    // VAD 正在录音
static volatile bool s_manual_mode = false;       // 按键手动模式（优先级 > VAD）

// 录音统计
static uint32_t s_record_chunks_enqueued = 0;
static uint32_t s_record_chunks_sent = 0;
static uint32_t s_record_chunks_dropped = 0;

// ================= PCM 播放环形缓冲区（接收服务端下发的音频） =================
// 服务端参数：50ms/chunk, 1600 bytes/chunk (16kHz × 16bit × mono × 50ms)
#define PLAYBACK_CHUNK_BYTES   1600   // 每个 chunk 的字节数
#define PLAYBACK_BUF_CHUNKS    60     // 环形缓冲区深度：60 × 50ms = 3000ms（吸收网络抖动）
#define PLAYBACK_PREBUF_CHUNKS 20      // 预缓冲阈值：攒满 20 chunk (1000ms) 后再启动播放
#define PLAYBACK_VOLUME_SHIFT  4      // 音量衰减：0=原始, 1=-6dB, 2=-12dB, 3=-18dB, 4=-24dB
#define PLAYBACK_SILENCE_TIMEOUT_MS  2000  // 连续静音超时自动停止 (ms)
// NOSPLIT 环形缓冲区：每项 = chunk + 内部头开销，多留余量
#define PLAYBACK_RINGBUF_SIZE  ((PLAYBACK_CHUNK_BYTES + 16) * PLAYBACK_BUF_CHUNKS)

static RingbufHandle_t s_pb_ringbuf = NULL; // FreeRTOS 环形缓冲区（SMP 安全，自带阻塞等待）
static TaskHandle_t  s_playback_task = NULL;
static volatile bool s_playback_running = false;

// 播放统计
static uint32_t s_pb_chunks_received = 0;
static uint32_t s_pb_chunks_played   = 0;
static uint32_t s_pb_overruns        = 0;  // 缓冲区满，丢弃
static uint32_t s_pb_underruns       = 0;  // 缓冲区空，填充静音

// ================= 预缓冲环形缓冲区：保存最近 PREBUF_CHUNKS 个 chunk =================
#define PREBUF_CHUNKS  15   // 15 × 20ms = 300ms 预缓冲
static int16_t s_prebuf[PREBUF_CHUNKS * (AUDIO_CHUNK_BYTES / 2)]; // 300ms 音频
static int     s_prebuf_head  = 0;   // 下一个写入位置
static int     s_prebuf_count = 0;   // 已存储的 chunk 数量

// 预缓冲快照：VAD 触发时保存，主循环创建队列后刷入
static int16_t s_prebuf_snapshot[PREBUF_CHUNKS * (AUDIO_CHUNK_BYTES / 2)];
static volatile int s_prebuf_flush_count = 0;  // 需要刷入的 chunk 数量

// 前向声明
static void audio_sender_task(void *arg);
static void playback_task(void *arg);
static void playback_start(void);
static void playback_stop(void);

// ================= 录音控制函数（抽取为独立函数） =================

/**
 * @brief 开始录音：发送 start 消息、创建队列和发送任务
 * @note I2S 流始终运行，此函数只负责建立发送通道
 */
static void start_recording(void)
{
    if (!ws_client_is_connected()) {
        ESP_LOGW(TAG, "[REC] WS 未连接，无法开始录音");
        return;
    }

    // 发送 start 消息
    lcd_st7789_show_text("Listening...");
    const char *start_msg = "{\"action\":\"start\"}";
    ws_client_send(start_msg, strlen(start_msg));

    // 重置统计
    s_record_chunks_enqueued = 0;
    s_record_chunks_sent = 0;
    s_record_chunks_dropped = 0;

    // 创建音频队列和发送任务
    if (!s_audio_queue) {
        s_audio_queue = xQueueCreate(AUDIO_QUEUE_LEN, AUDIO_CHUNK_BYTES);
    }
    if (s_audio_queue && !s_audio_sender_task) {
        s_sender_running = true;
        xTaskCreate(audio_sender_task, "audio_send", 4096, NULL, 6, &s_audio_sender_task);
    }

    ESP_LOGI(TAG, "[REC] 录音已开始");
}

/**
 * @brief 停止录音：停止发送任务、排空队列、发送 stop 消息、清理
 * @note 不停止 I2S 流（VAD 需要持续监听）
 */
static void stop_recording(void)
{
    // 1. 等待发送任务排空队列（最多 2 秒）
    if (s_sender_running) {
        s_sender_running = false;
        for (int i = 0; i < 200 && s_audio_sender_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // 2. 发送 stop 消息（此时所有音频数据已发送完毕）
    if (ws_client_is_connected()) {
        const char *stop_msg = "{\"action\":\"stop\"}";
        ws_client_send(stop_msg, strlen(stop_msg));
    }

    // 3. 打印录音统计
    ESP_LOGI(TAG, "[REC] 统计: 入队=%lu, 发送=%lu, 丢弃=%lu, 约 %.1f 秒",
             (unsigned long)s_record_chunks_enqueued,
             (unsigned long)s_record_chunks_sent,
             (unsigned long)s_record_chunks_dropped,
             s_record_chunks_sent * RECORD_CHUNK_MS / 1000.0f);

    // 4. 清理队列
    if (s_audio_queue) {
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
    }

    s_vad_recording = false;

    if (s_lcd_ok) lcd_st7789_show_text("Standby");
    ESP_LOGI(TAG, "[REC] 录音已停止");
}

// ================= 音频发送任务 =================

/**
 * @brief 独立的 WebSocket 音频发送任务
 */
static void audio_sender_task(void *arg)
{
    uint8_t *buf = (uint8_t *)heap_caps_malloc(AUDIO_CHUNK_BYTES, MALLOC_CAP_INTERNAL);
    if (!buf) {
        ESP_LOGE(TAG, "[SENDER] 缓冲区分配失败");
        s_sender_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[SENDER] 音频发送任务启动");

    while (s_sender_running) {
        if (xQueueReceive(s_audio_queue, buf, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (ws_client_is_connected()) {
                ws_client_send_bin(buf, AUDIO_CHUNK_BYTES);
                s_record_chunks_sent++;
            }
        }
    }

    // 排空队列中剩余数据
    while (xQueueReceive(s_audio_queue, buf, 0) == pdTRUE) {
        if (ws_client_is_connected()) {
            ws_client_send_bin(buf, AUDIO_CHUNK_BYTES);
            s_record_chunks_sent++;
        }
    }

    heap_caps_free(buf);
    ESP_LOGI(TAG, "[SENDER] 音频发送任务退出, 已发送 %lu chunks",
             (unsigned long)s_record_chunks_sent);
    s_audio_sender_task = NULL;
    vTaskDelete(NULL);
}

// ================= PCM 播放任务：从 RingBuf 取数据送入 I2S =================

/**
 * @brief PCM 播放任务：持续从 RingBuf 读取 chunk 并写入 I2S
 *        使用 xRingbufferReceive 阻塞等待，超时则填充静音防止 I2S underrun
 */
static void playback_task(void *arg)
{
    static int16_t silence_chunk[PLAYBACK_CHUNK_BYTES / 2]; // 全零静音帧
    memset(silence_chunk, 0, sizeof(silence_chunk));

    int16_t chunk_buf[PLAYBACK_CHUNK_BYTES / 2]; // 从 ringbuf 拷贝后做音量衰减
    const TickType_t chunk_ticks = pdMS_TO_TICKS(50); // 每个 chunk 50ms
    const int silence_limit = PLAYBACK_SILENCE_TIMEOUT_MS / 50;
    int consecutive_silence = 0;

    ESP_LOGI(TAG, "[PLAYBACK] 播放任务启动 (chunk=%d bytes, ringbuf=%d chunks)",
             PLAYBACK_CHUNK_BYTES, PLAYBACK_BUF_CHUNKS);

    while (s_playback_running) {
        TickType_t iter_start = xTaskGetTickCount();

        // 阻塞等待数据，最多等一个 chunk 时间 (50ms)
        // ringbuf 内部处理 SMP 安全，无需 spinlock
        size_t item_size = 0;
        const void *item = xRingbufferReceive(s_pb_ringbuf, &item_size, chunk_ticks);

        if (item == NULL) {
            // 超时：缓冲区空，填充静音避免 I2S underrun
            max98357a_write(silence_chunk, PLAYBACK_CHUNK_BYTES / 2, 100);
            s_pb_underruns++;
            consecutive_silence++;
            if (consecutive_silence <= 3 || (consecutive_silence % 20 == 0)) {
                ESP_LOGW(TAG, "[PLAYBACK] 缓冲区空, 静音填充 (累计 %lu 次)",
                         (unsigned long)s_pb_underruns);
            }
            if (consecutive_silence >= silence_limit) {
                ESP_LOGI(TAG, "[PLAYBACK] 服务端播放完毕, 连续静音 %d 次, 自动停止",
                         consecutive_silence);
                s_playback_running = false;
                break;
            }
            continue;
        }

        // 收到新数据，重置静音计数
        consecutive_silence = 0;

        // 拷贝到本地缓冲区后做音量衰减（ringbuf 返回的是只读指针）
        memcpy(chunk_buf, item, PLAYBACK_CHUNK_BYTES);
        vRingbufferReturnItem(s_pb_ringbuf, (void *)item);

#if PLAYBACK_VOLUME_SHIFT > 0
        for (int i = 0; i < PLAYBACK_CHUNK_BYTES / 2; i++) {
            chunk_buf[i] >>= PLAYBACK_VOLUME_SHIFT;
        }
#endif
        max98357a_write(chunk_buf, PLAYBACK_CHUNK_BYTES / 2, 100);
        s_pb_chunks_played++;

        // 速率限制：确保每次迭代耗时 >= 50ms（匹配音频实时时长）
        TickType_t elapsed = xTaskGetTickCount() - iter_start;
        if (elapsed < chunk_ticks) {
            vTaskDelay(chunk_ticks - elapsed);
        }
    }

    ESP_LOGI(TAG, "[PLAYBACK] 播放任务退出, 接收=%lu, 播放=%lu, overrun=%lu, underrun=%lu",
             (unsigned long)s_pb_chunks_received,
             (unsigned long)s_pb_chunks_played,
             (unsigned long)s_pb_overruns,
             (unsigned long)s_pb_underruns);
    s_playback_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 启动播放：创建 RingBuf，重置统计
 */
static void playback_start(void)
{
    if (s_playback_running) return;

    s_pb_chunks_received = 0;
    s_pb_chunks_played   = 0;
    s_pb_overruns  = 0;
    s_pb_underruns = 0;

    if (!s_pb_ringbuf) {
        s_pb_ringbuf = xRingbufferCreate(PLAYBACK_RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
        if (!s_pb_ringbuf) {
            ESP_LOGE(TAG, "[PLAYBACK] RingBuf 创建失败");
            return;
        }
    }

    s_playback_running = true;
    ESP_LOGI(TAG, "[PLAYBACK] 预缓冲开始 (阈值=%d chunks)", PLAYBACK_PREBUF_CHUNKS);
}

/**
 * @brief 停止播放：等待任务退出并清理 RingBuf
 */
static void playback_stop(void)
{
    if (!s_playback_running && s_playback_task == NULL && s_pb_ringbuf == NULL) return;

    s_playback_running = false;
    // 唤醒播放任务（如果它还在等 ringbuf）：发送完整 chunk 大小的 dummy 防止 memcpy 越界
    if (s_pb_ringbuf && s_playback_task != NULL) {
        uint8_t dummy[PLAYBACK_CHUNK_BYTES];
        memset(dummy, 0, sizeof(dummy));
        xRingbufferSend(s_pb_ringbuf, dummy, PLAYBACK_CHUNK_BYTES, 0);
    }
    // 等待任务退出
    if (s_playback_task != NULL) {
        for (int i = 0; i < 100 && s_playback_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    // 清理 RingBuf
    if (s_pb_ringbuf) {
        vRingbufferDelete(s_pb_ringbuf);
        s_pb_ringbuf = NULL;
    }
    ESP_LOGI(TAG, "[PLAYBACK] 播放已停止");
}

/**
 * @brief WebSocket 二进制回调：将 PCM 数据写入 RingBuf
 *        由 websocket 事件线程调用，ringbuf 内部处理 SMP 安全
 */
static void on_ws_binary(const uint8_t *data, size_t len)
{
    s_pb_chunks_received++;

    // 只接受预期大小的 chunk（防止异常数据）
    if (len != PLAYBACK_CHUNK_BYTES) {
        ESP_LOGW(TAG, "[PLAYBACK] 收到非预期大小: %d (期望 %d)", (int)len, PLAYBACK_CHUNK_BYTES);
        if (len > PLAYBACK_CHUNK_BYTES) len = PLAYBACK_CHUNK_BYTES;
    }

    if (!s_playback_running) {
        playback_start();
    }

    // 写入 RingBuf（SMP 安全，无需 spinlock）
    if (xRingbufferSend(s_pb_ringbuf, data, PLAYBACK_CHUNK_BYTES, 0) != pdTRUE) {
        // 缓冲区满：丢弃最旧数据后重试
        size_t old_size;
        void *old = xRingbufferReceive(s_pb_ringbuf, &old_size, 0);
        if (old) vRingbufferReturnItem(s_pb_ringbuf, old);
        xRingbufferSend(s_pb_ringbuf, data, PLAYBACK_CHUNK_BYTES, 0);
        s_pb_overruns++;
        if (s_pb_overruns <= 3 || (s_pb_overruns % 20 == 0)) {
            ESP_LOGW(TAG, "[PLAYBACK] 缓冲区满, 丢弃旧数据 (累计 %lu 次)",
                     (unsigned long)s_pb_overruns);
        }
    }

    // 预缓冲阶段：攒够 PREBUF_CHUNKS 后启动播放任务
    size_t free_bytes = xRingbufferGetCurFreeSize(s_pb_ringbuf);
    size_t used_bytes = PLAYBACK_RINGBUF_SIZE - free_bytes;
    int buffered_chunks = (int)(used_bytes / PLAYBACK_CHUNK_BYTES);
    if (!s_playback_task && buffered_chunks >= PLAYBACK_PREBUF_CHUNKS) {
        ESP_LOGI(TAG, "[PLAYBACK] 预缓冲完成 (~%d chunks), 启动播放任务", buffered_chunks);
        xTaskCreate(playback_task, "pcm_playback", 4096, NULL, 7, &s_playback_task);
    }
}

// ================= 音频流回调：VAD 分析 + 预缓冲 + 入队 =================
static uint32_t s_debug_frame_count = 0;  // 调试用：帧计数器
static vad_state_t s_prev_vad_state = VAD_STATE_IDLE;  // 上一帧的 VAD 状态

static void audio_stream_callback(const int16_t *samples, size_t frame_count, void *user_ctx)
{
    // 1. 始终进行 VAD 分析
    vad_process(samples, frame_count);
    vad_state_t cur_state = vad_get_state();

    // 2. 检测 VAD 刚触发 (IDLE → LISTENING)：保存预缓冲快照
    if (cur_state == VAD_STATE_LISTENING && s_prev_vad_state == VAD_STATE_IDLE) {
        int count = s_prebuf_count < PREBUF_CHUNKS ? s_prebuf_count : PREBUF_CHUNKS;
        int start = (s_prebuf_head - count + PREBUF_CHUNKS) % PREBUF_CHUNKS;
        for (int i = 0; i < count; i++) {
            int idx = (start + i) % PREBUF_CHUNKS;
            memcpy(&s_prebuf_snapshot[i * (AUDIO_CHUNK_BYTES / 2)],
                   &s_prebuf[idx * (AUDIO_CHUNK_BYTES / 2)],
                   AUDIO_CHUNK_BYTES);
        }
        s_prebuf_flush_count = count;
        ESP_LOGI(TAG, "[PREBUF] 快照已保存, %d chunks (%dms)", count, count * 20);
    }
    s_prev_vad_state = cur_state;

    // 3. 非录音状态：存入预缓冲环形缓冲区
    if (!s_vad_recording && !s_manual_mode) {
        memcpy(&s_prebuf[s_prebuf_head * (AUDIO_CHUNK_BYTES / 2)],
               samples, AUDIO_CHUNK_BYTES);
        s_prebuf_head = (s_prebuf_head + 1) % PREBUF_CHUNKS;
        if (s_prebuf_count < PREBUF_CHUNKS) s_prebuf_count++;
    }

    // 4. 录音状态：入队发送
    if ((s_vad_recording || s_manual_mode) && s_audio_queue && s_sender_running) {
        if (xQueueSend(s_audio_queue, samples, 0) != pdTRUE) {
            s_record_chunks_dropped++;
            if (s_record_chunks_dropped <= 5 || (s_record_chunks_dropped % 50 == 0)) {
                ESP_LOGW(TAG, "[AUDIO] 队列满, 已丢弃 %lu chunks",
                         (unsigned long)s_record_chunks_dropped);
            }
        } else {
            s_record_chunks_enqueued++;
        }
    }

    // 5. 调试日志
    s_debug_frame_count++;
    if (s_debug_frame_count % 50 == 0) {
        int vol = vad_get_volume();
        const char *state_str = "IDLE";
        switch (cur_state) {
            case VAD_STATE_LISTENING: state_str = "LISTENING"; break;
            case VAD_STATE_ENDING:    state_str = "ENDING";    break;
            default: break;
        }
        ESP_LOGI(TAG, "[DEBUG] vol=%d, vad=%s, recording=%d", vol, state_str, (int)s_vad_recording);
    }
}

// ================= VAD 事件回调：仅设置 EventGroup bit =================
static void on_vad_event(vad_event_t event, int volume, void *user_ctx)
{
    if (event == VAD_EVENT_START) {
        ESP_LOGI(TAG, "[VAD-EVENT] START, volume=%d", volume);
        xEventGroupSetBits(s_event_group, EVENT_VAD_START);
    } else {
        ESP_LOGI(TAG, "[VAD-EVENT] STOP, volume=%d", volume);
        xEventGroupSetBits(s_event_group, EVENT_VAD_STOP);
    }
}

// ================= 按键事件回调：手动模式（优先级 > VAD） =================
static void on_key_event(key_event_t event, void *user_ctx)
{
    if (event == KEY_EVENT_PRESSED) {
        ESP_LOGI(TAG, "[KEY] 按下事件 (手动录音)");

        // 暂停 VAD，进入手动模式
        vad_set_enabled(false);

        // 如果 VAD 正在录音，先停掉
        if (s_vad_recording) {
            stop_recording();
            vad_force_stop();
        }

        s_manual_mode = true;

        if (ws_client_is_connected()) {
            if (!inmp441_is_streaming()) {
                inmp441_start_stream(audio_stream_callback, NULL, RECORD_CHUNK_MS);
            }
            start_recording();
        } else {
            lcd_st7789_show_text("WS Disconnected");
            ESP_LOGW(TAG, "[KEY] WS 未连接，忽略录音请求");
            s_manual_mode = false;
            vad_set_enabled(true);
        }
    }
    else if (event == KEY_EVENT_RELEASED) {
        ESP_LOGI(TAG, "[KEY] 松开事件");

        if (s_manual_mode) {
            stop_recording();
            s_manual_mode = false;

            // 冷却期后恢复 VAD
            vTaskDelay(pdMS_TO_TICKS(500));
            vad_set_enabled(true);
        }
    }
}

// ================= 回调：仅设置 EventGroup bit，不做业务逻辑 =================

static void on_ws_status(bool connected) {
    xEventGroupSetBits(s_event_group,
        connected ? EVENT_WS_CONNECTED : EVENT_WS_DISCONNECTED);
}

static void on_ws_message(const char *data, size_t len) {
    ESP_LOGI(TAG, "[WS] 收到服务器消息 (%d 字节): %.*s", (int)len, (int)len, data);
    // TODO: 在此处理服务器下发的指令或数据
}

// ================= ESP-IDF 事件回调：仅设置 bit =================

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_event_group, EVENT_NETWORK_CONNECTED);
    }
}

static void on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_event_group, EVENT_NETWORK_DISCONNECTED);
    }
}

// ================= 1秒定时心跳：用于超时检测 =================

static void timeout_timer_callback(void *arg) {
    xEventGroupSetBits(s_event_group, EVENT_TIMEOUT_TICK);
}


extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== Application Start ===");

    // 创建 EventGroup
    s_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "Step 1: Init NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "Step 1: NVS OK");

    ESP_LOGI(TAG, "Step 2: Init Netif...");
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(ret));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Step 2: Netif OK");

    ESP_LOGI(TAG, "Step 3: Init Event Loop...");
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop init failed: %s", esp_err_to_name(ret));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Step 3: Event Loop OK");

    // 注册 ESP-IDF 事件回调（仅设置 bit）
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_disconnect, NULL));

    // 创建 1 秒定时心跳（用于连接超时检测）
    esp_timer_create_args_t timer_args = {
        .callback = timeout_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "timeout_tick",
        .skip_unhandled_events = true
    };
    esp_timer_handle_t timeout_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timeout_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timeout_timer, 1000000)); // 1秒

    ESP_LOGI(TAG, "Step 4: Init Speaker...");
    bool speaker_ok = (max98357a_init() == ESP_OK);
    if (speaker_ok) {
        max98357a_play_wav(enter_config_wav, enter_config_wav_len);
    }
    ESP_LOGI(TAG, "Step 4: Speaker %s", speaker_ok ? "OK" : "FAILED");

    ESP_LOGI(TAG, "Step 4.5: Init Microphone (INMP441)...");
    bool mic_ok = (inmp441_init() == ESP_OK);
    ESP_LOGI(TAG, "Step 4.5: Microphone %s", mic_ok ? "OK" : "FAILED");
    if (mic_ok) {
        vTaskDelay(pdMS_TO_TICKS(100));  // 等待 I2S DMA 稳定
        // 测试麦克风: 读取音频并打印音量
        int16_t test_buf[1600];  // 16kHz * 0.1s = 1600 samples
        size_t frames_read = 0;
        if (inmp441_read(test_buf, 1600, 500, &frames_read) == ESP_OK) {
            int64_t sum_sq = 0;
            for (size_t i = 0; i < frames_read; i++) {
                int32_t s = test_buf[i];
                sum_sq += s * s;
            }
            int volume = (frames_read > 0) ? (int)sqrtf((float)(sum_sq / frames_read)) : 0;
            ESP_LOGI(TAG, "Mic test: read %d frames, RMS volume=%d", (int)frames_read, volume);
        }
    }

    // 初始化 VAD（使用默认参数）
    vad_init(NULL, on_vad_event, NULL);

    // I2S 初始化后立即启动持续流式采集（VAD 需要持续监听）
    if (mic_ok) {
        inmp441_start_stream(audio_stream_callback, NULL, RECORD_CHUNK_MS);
        ESP_LOGI(TAG, "Step 4.5: I2S 持续流已启动（VAD 监听模式）");
    }

    ESP_LOGI(TAG, "Step 4.6: Init Key...");
    key_init();
    key_set_callback(on_key_event, NULL);
    ESP_LOGI(TAG, "Step 4.6: Key OK");

    ESP_LOGI(TAG, "Step 5: Init LCD...");
    bool lcd_ok = (lcd_st7789_init() == ESP_OK);
    if (lcd_ok) {
        lcd_ok = (lcd_st7789_init_lvgl() == ESP_OK);
    }
    if (lcd_ok) {
        lcd_ok = (lcd_st7789_start_lvgl_task() == ESP_OK);
    }
    ESP_LOGI(TAG, "Step 5: LCD %s, internal RAM: %d bytes",
             lcd_ok ? "OK" : "FAILED",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (lcd_ok) {
        lcd_st7789_show_text("Standby");
    }
    s_lcd_ok = lcd_ok;

    ESP_LOGI(TAG, "Step 6: Init Wi-Fi Manager...");
    auto& wifi_manager = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.ssid_prefix = "Chenjinxiao";
    config.language = "zh-CN";

    bool wifi_ok = wifi_manager.Initialize(config);
    ESP_LOGI(TAG, "Step 6: Wi-Fi Manager: %s", wifi_ok ? "OK" : "FAIL");
    if (!wifi_ok) while(1) vTaskDelay(pdMS_TO_TICKS(1000));

    auto& ssid_list = SsidManager::GetInstance().GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGI(TAG, "Step 7: Start Config AP");
        wifi_manager.StartConfigAp();
        if (lcd_ok) lcd_st7789_show_text("请连接热点并配置 Wi-Fi");
    } else {
        ESP_LOGI(TAG, "Step 7: Start Station");
        wifi_manager.StartStation();
        if (lcd_ok) lcd_st7789_show_text("Connecting");
    }

    // ================= EventGroup 驱动的主循环 =================
    const EventBits_t ALL_EVENTS =
        EVENT_NETWORK_CONNECTED |
        EVENT_NETWORK_DISCONNECTED |
        EVENT_WS_CONNECTED |
        EVENT_WS_DISCONNECTED |
        EVENT_TIMEOUT_TICK |
        EVENT_VAD_START |
        EVENT_VAD_STOP;

    int timeout_counter = 0;

    while (true) {
        // 阻塞等待事件，无事件时零 CPU 消耗
        EventBits_t bits = xEventGroupWaitBits(
            s_event_group, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        // ---------- 获取到 IP ----------
        if (bits & EVENT_NETWORK_CONNECTED) {
            ESP_LOGI(TAG, "网络就绪，启动 WebSocket");
            timeout_counter = 0;

            if (s_lcd_ok) {
                lcd_st7789_set_status_text("ONLINE");
                lcd_st7789_show_text("Connected");
                lcd_st7789_set_battery_percent(80);
            }

            if (!s_ws_started) {
                if (ws_client_init(WS_SERVER_URI, on_ws_status, on_ws_message, on_ws_binary) == ESP_OK) {
                    ws_client_start();
                    s_ws_started = true;
                    if (s_lcd_ok) lcd_st7789_show_text("WS Connecting");
                }
            }
        }

        // ---------- WiFi 断开 ----------
        if (bits & EVENT_NETWORK_DISCONNECTED) {
            ESP_LOGW(TAG, "WiFi 断开，清理 WebSocket");
            if (s_ws_started) {
                ws_client_stop();
                s_ws_started = false;
            }
            if (s_lcd_ok) {
                lcd_st7789_set_status_text("OFFLINE");
                lcd_st7789_show_text("Disconnected");
            }
        }

        // ---------- WebSocket 已连接 ----------
        if (bits & EVENT_WS_CONNECTED) {
            ESP_LOGI(TAG, "[WS] 已连接到后端服务器");
            const char *hello = "{\"type\":\"device_online\"}";
            ws_client_send(hello, strlen(hello));
            if (s_lcd_ok) lcd_st7789_set_status_text("WS OK");
        }

        // ---------- WebSocket 断开 ----------
        if (bits & EVENT_WS_DISCONNECTED) {
            ESP_LOGW(TAG, "[WS] 与后端服务器断开");

            // 停止 PCM 播放（服务端不再下发音频）
            playback_stop();

            // 如果正在录音（VAD 或手动），强制停止
            vad_force_stop();
            s_vad_recording = false;
            s_manual_mode = false;
            vad_set_enabled(true);  // 恢复 VAD 监听

            // 停止发送任务（但不停止 I2S 流，VAD 需要持续监听）
            if (s_sender_running) {
                s_sender_running = false;
                for (int i = 0; i < 50 && s_audio_sender_task != NULL; i++) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            if (s_audio_queue) {
                vQueueDelete(s_audio_queue);
                s_audio_queue = NULL;
            }
            if (s_lcd_ok) {
                lcd_st7789_set_status_text("ONLINE");
                lcd_st7789_show_text("Standby");
            }
        }

        // ---------- VAD 检测到语音 ----------
        if (bits & EVENT_VAD_START) {
            if (s_manual_mode) {
                // 手动模式优先，忽略 VAD 触发
            } else if (!ws_client_is_connected()) {
                ESP_LOGW(TAG, "[VAD] WS 未连接，忽略语音触发");
                vad_force_stop();
                if (s_lcd_ok) lcd_st7789_show_text("WS not ready");
            } else {
                ESP_LOGI(TAG, "[VAD] 检测到语音，开始录音");
                s_vad_recording = true;
                start_recording();

                // 刷入预缓冲快照（VAD 触发前 300ms 的音频）
                if (s_prebuf_flush_count > 0 && s_audio_queue) {
                    int count = s_prebuf_flush_count;
                    for (int i = 0; i < count; i++) {
                        xQueueSend(s_audio_queue,
                                   &s_prebuf_snapshot[i * (AUDIO_CHUNK_BYTES / 2)],
                                   0);
                        s_record_chunks_enqueued++;
                    }
                    ESP_LOGI(TAG, "[PREBUF] 已刷入 %d chunks 预缓冲", count);
                    s_prebuf_flush_count = 0;
                }
            }
        }

        // ---------- VAD 确认静音 ----------
        if (bits & EVENT_VAD_STOP) {
            if (s_vad_recording && !s_manual_mode) {
                ESP_LOGI(TAG, "[VAD] 确认静音，停止录音");
                stop_recording();
            }
        }

        // ---------- 1秒心跳：连接超时回退 AP ----------
        if (bits & EVENT_TIMEOUT_TICK) {
            if (!wifi_manager.IsConnected() && !wifi_manager.IsConfigMode()) {
                auto& ssid_list = SsidManager::GetInstance().GetSsidList();
                if (!ssid_list.empty() && timeout_counter == 0) {
                    ESP_LOGI(TAG, "配网完成，启动 Station 模式连接已保存的 WiFi");
                    wifi_manager.StartStation();
                    if (s_lcd_ok) lcd_st7789_show_text("Connecting...");
                    timeout_counter = 1;
                    continue;
                }

                timeout_counter++;
                if (timeout_counter > 60) {
                    ESP_LOGW(TAG, "Timeout -> Switch AP");
                    if (lcd_ok) lcd_st7789_show_text("Fail->AP");
                    wifi_manager.StopStation();
                    wifi_manager.StartConfigAp();
                    timeout_counter = 0;
                } else if (timeout_counter % 10 == 0 && lcd_ok) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "Try %ds", timeout_counter);
                    lcd_st7789_show_text(buf);
                }
            } else {
                timeout_counter = 0;
            }
        }
    }
}
