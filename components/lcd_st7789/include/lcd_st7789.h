#ifndef LCD_ST7789_H
#define LCD_ST7789_H

#include "driver/gpio.h"
#include "esp_err.h"

#define LCD_ST7789_WIDTH  240
#define LCD_ST7789_HEIGHT 240

// BLK
#define LCD_ST7789_PIN_BL   GPIO_NUM_42
#define LCD_ST7789_PIN_CS   GPIO_NUM_41
#define LCD_ST7789_PIN_DC   GPIO_NUM_40
// RES
#define LCD_ST7789_PIN_RST  GPIO_NUM_39
// SPI pins
#define LCD_ST7789_PIN_MOSI GPIO_NUM_38
// SCLK
#define LCD_ST7789_PIN_SCLK GPIO_NUM_37





#define LCD_ST7789_X_OFFSET 0
#define LCD_ST7789_Y_OFFSET 0

#define LCD_COLOR_BLACK 0x0000
#define LCD_COLOR_WHITE 0xFFFF
#define LCD_COLOR_RED   0xF800
#define LCD_COLOR_GREEN 0x07E0
#define LCD_COLOR_BLUE  0x001F

esp_err_t lcd_st7789_init(void);
esp_err_t lcd_st7789_fill_screen(uint16_t color);
esp_err_t lcd_st7789_draw_string(int x, int y, const char *text, uint16_t color, uint16_t bg_color, int scale);

#endif
