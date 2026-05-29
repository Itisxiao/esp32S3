#include "led.h"

void led_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };

    gpio_config(&io_conf);
    led_off();
}

void led_set(led_state_t state)
{
    gpio_set_level(LED_GPIO, state);
}

void led_on(void)
{
    led_set(LED_ON);
}

void led_off(void)
{
    led_set(LED_OFF);
}

void led_toggle(void)
{
    gpio_set_level(LED_GPIO, !gpio_get_level(LED_GPIO));
}
