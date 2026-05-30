#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd_st7789.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ST7789 LCD example");

    ESP_ERROR_CHECK(lcd_st7789_init());
    ESP_ERROR_CHECK(lcd_st7789_show_text("hello world"));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
