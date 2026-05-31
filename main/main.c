#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd_st7789.h"      // 你的 LCD 初始化头文件
#include "wifi_config_portal.h"   // 上面的头文件
#include "esp_task_wdt.h"   // 看门狗头文件

static const char *TAG = "main";

void app_main(void)
{
   ESP_LOGI(TAG, "Starting application...");
   
    ESP_ERROR_CHECK(wifi_portal_init());
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI(TAG, "Starting ST7789 LCD...");
    ESP_ERROR_CHECK(lcd_st7789_init());
    ESP_ERROR_CHECK(lcd_st7789_show_text("hello world"));
    lcd_st7789_set_status_text("ONLINE");
    lcd_st7789_set_battery_percent(80);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}