#include "key.h"


void key_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << KEY_GPIO),
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };

    gpio_config(&io_conf);
}

uint8_t key_scan(uint8_t mode)
{
    uint8_t keyvalue = 0;
    static uint8_t key_boot = 1;
    if(mode){
        key_boot = 1;
    }

    if(key_boot && (gpio_get_level(KEY_GPIO) == 0)){
        vTaskDelay(pdMS_TO_TICKS(10));
        key_boot = 0;
        if(gpio_get_level(KEY_GPIO) == 0){
            keyvalue = 1;
        }
    }
    else if(gpio_get_level(KEY_GPIO) == 1){
        key_boot = 1;
    }
    return keyvalue;


}