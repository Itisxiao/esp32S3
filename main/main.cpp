#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
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
#include <cstring>
#include <cstdio>
#include <cmath>

static const char *TAG = "main";

// TODO: 替换为你的后端服务器地址
#define WS_SERVER_URI "ws://192.168.0.101:8080//ws/audio"

// ================= EventGroup 事件位定义 =================
#define EVENT_NETWORK_CONNECTED   BIT0   // 获取到 IP
#define EVENT_NETWORK_DISCONNECTED BIT1   // WiFi 断开
#define EVENT_WS_CONNECTED        BIT2   // WebSocket 已连接
#define EVENT_WS_DISCONNECTED     BIT3   // WebSocket 断开
#define EVENT_TIMEOUT_TICK        BIT4   // 1秒定时心跳

// ================= 录音参数 =================
#define RECORD_CHUNK_MS       20    // 每次发送 20ms 音频

// ================= 全局状态 =================
static EventGroupHandle_t s_event_group = NULL;
static bool s_lcd_ok = false;
static bool s_ws_started = false;

// ================= 音频流回调：每 chunk 由 inmp441 后台任务调用 =================
static void audio_stream_callback(const int16_t *samples, size_t frame_count, void *user_ctx)
{
    if (ws_client_is_connected()) {
        ws_client_send_bin((const uint8_t *)samples, frame_count * sizeof(int16_t));
    }
}

// ================= 按键事件回调（由消抖任务调用，可安全使用阻塞 API） =================
static void on_key_event(key_event_t event, void *user_ctx)
{
    if (event == KEY_EVENT_PRESSED) {
        ESP_LOGI(TAG, "[KEY] 按下事件");

        if (ws_client_is_connected()) {
            const char *start_msg = "{\"action\":\"start\"}";
            ws_client_send(start_msg, strlen(start_msg));

            if (!inmp441_is_streaming()) {
                inmp441_start_stream(audio_stream_callback, NULL, RECORD_CHUNK_MS);
            }
        } else {
            ESP_LOGW(TAG, "[KEY] WS 未连接，忽略录音请求");
        }
    }
    else if (event == KEY_EVENT_RELEASED) {
        ESP_LOGI(TAG, "[KEY] 松开事件");

        if (inmp441_is_streaming()) {
            inmp441_stop_stream();
        }

        if (ws_client_is_connected()) {
            const char *stop_msg = "{\"action\":\"stop\"}";
            ws_client_send(stop_msg, strlen(stop_msg));
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
        //max98357a_play_tone(1000, 120, 3000);
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
        lcd_st7789_show_text("Booting");
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
    // 所有业务逻辑集中在此，回调仅负责设置 bit
    const EventBits_t ALL_EVENTS =
        EVENT_NETWORK_CONNECTED |
        EVENT_NETWORK_DISCONNECTED |
        EVENT_WS_CONNECTED |
        EVENT_WS_DISCONNECTED |
        EVENT_TIMEOUT_TICK;

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
                if (ws_client_init(WS_SERVER_URI, on_ws_status, on_ws_message) == ESP_OK) {
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
            // 如果正在录音，强制停止
            if (inmp441_is_streaming()) {
                inmp441_stop_stream();
            }
            if (s_lcd_ok) lcd_st7789_set_status_text("ONLINE");
        }

        // ---------- 1秒心跳：连接超时回退 AP ----------
        if (bits & EVENT_TIMEOUT_TICK) {
            if (!wifi_manager.IsConnected() && !wifi_manager.IsConfigMode()) {
                // 配网完成后退出 AP 模式，但 Station 未启动 —— 立即启动 Station
                auto& ssid_list = SsidManager::GetInstance().GetSsidList();
                if (!ssid_list.empty() && timeout_counter == 0) {
                    ESP_LOGI(TAG, "配网完成，启动 Station 模式连接已保存的 WiFi");
                    wifi_manager.StartStation();
                    if (s_lcd_ok) lcd_st7789_show_text("Connecting...");
                    timeout_counter = 1;  // 避免重复触发
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
