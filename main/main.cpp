#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd_st7789.h"
#include "wifi_station.h"
#include "wifi_configuration_ap.h"
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
        wifi_manager.StartConfigAp();
    } else {
        wifi_manager.StartStation();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Starting ST7789 LCD...");
    ESP_ERROR_CHECK(lcd_st7789_init());
    ESP_ERROR_CHECK(lcd_st7789_show_text("hello world"));
    lcd_st7789_set_battery_percent(80);

    while (1) {
        if (wifi_manager.IsConnected()) {
            lcd_st7789_set_status_text("ONLINE");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}