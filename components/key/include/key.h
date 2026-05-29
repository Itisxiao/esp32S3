#ifndef __KEY_H__
#define __KEY_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define KEY_GPIO GPIO_NUM_0

void key_init(void);

uint8_t key_scan(uint8_t mode);


#endif 