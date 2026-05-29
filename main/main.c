#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting LED example");
    led_init();
    
    while (1) {
        led_on();
        ESP_LOGI(TAG, "LED ON");
        vTaskDelay(pdMS_TO_TICKS(2000));

        led_off();
        ESP_LOGI(TAG, "LED OFF");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
