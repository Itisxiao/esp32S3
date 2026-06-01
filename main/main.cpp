#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd_st7789.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

static const char *TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting application...");

    auto& wifi_manager = WifiManager::GetInstance();

    WifiManagerConfig config;
    config.ssid_prefix = "Chenjinxiao";
    config.language = "zh-CN";
    wifi_manager.Initialize(config);

    auto& ssid_list = SsidManager::GetInstance().GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGI(TAG, "No saved WiFi, starting config AP");
        wifi_manager.StartConfigAp();
    } else {
        ESP_LOGI(TAG, "Found %d saved WiFi(s), trying to connect", ssid_list.size());
        wifi_manager.StartStation();
    }

    int connection_timeout = 0;
    bool lcd_initialized = false;

    while (1) {
        if (wifi_manager.IsConnected()) {
            // WiFi连接成功，初始化LCD
            if (!lcd_initialized) {
                ESP_LOGI(TAG, "WiFi connected, initializing LCD...");
                esp_err_t lcd_err = lcd_st7789_init();
                if (lcd_err == ESP_OK) {
                    ESP_ERROR_CHECK(lcd_st7789_show_text("hello world"));
                    lcd_st7789_set_battery_percent(80);
                    lcd_st7789_set_status_text("ONLINE");
                    lcd_initialized = true;
                    ESP_LOGI(TAG, "LCD initialized successfully");
                } else {
                    ESP_LOGW(TAG, "LCD init failed: %s", esp_err_to_name(lcd_err));
                }
            }
            connection_timeout = 0;
        } else if (!wifi_manager.IsConfigMode() && connection_timeout++ > 60) {
            ESP_LOGW(TAG, "Failed to connect after 60s, starting config AP");
            wifi_manager.StopStation();
            wifi_manager.StartConfigAp();
            connection_timeout = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}