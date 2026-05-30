#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd_st7789.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ST7789 LCD example");

    ESP_ERROR_CHECK(lcd_st7789_init());
    ESP_ERROR_CHECK(lcd_st7789_draw_string(36, 104, "hello world",
                                           LCD_COLOR_WHITE,
                                           LCD_COLOR_BLACK,
                                           3));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
