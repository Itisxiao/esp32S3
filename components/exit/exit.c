#include "exit.h"

void IRAM_ATTR exit_gpio_isr_handler(void *arg){
    uint32_t gpio_num = (uint32_t) arg;
    if(gpio_num == EXIT_GPIO){
        led_toggle();
    }
}

void exit_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << EXIT_GPIO),
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };

    gpio_config(&io_conf);

    gpio_install_isr_service(0);

    gpio_isr_handler_add(EXIT_GPIO, exit_gpio_isr_handler, EXIT_GPIO);
}