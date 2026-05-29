#ifndef LED_H
#define LED_H

#include <stdbool.h>
#include "driver/gpio.h"

#define LED_GPIO GPIO_NUM_1

typedef enum {
    LED_OFF = 0,
    LED_ON = 1,
} led_state_t;

void led_init(void);
void led_set(led_state_t state);
void led_on(void);
void led_off(void);
void led_toggle(void);

#endif
