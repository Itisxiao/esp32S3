#ifndef LCD_ST7789_H
#define LCD_ST7789_H

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/gpio.h"
#include "esp_err.h"

#define LCD_ST7789_WIDTH  240
#define LCD_ST7789_HEIGHT 240

/*
 * ESP32-S3 with Octal PSRAM/Flash uses GPIO33..37 for SPI0/1 high data lines
 * and DQS. Do not put the LCD on those pins when CONFIG_SPIRAM_MODE_OCT=y.
 * Rewire the ST7789 to these GPIOs, or change all pins below to another
 * non-reserved group that matches your board.
 */
#define LCD_ST7789_PIN_BL   GPIO_NUM_42
#define LCD_ST7789_PIN_CS   GPIO_NUM_41
#define LCD_ST7789_PIN_DC   GPIO_NUM_40
#define LCD_ST7789_PIN_RST  GPIO_NUM_45
#define LCD_ST7789_PIN_MOSI GPIO_NUM_47
#define LCD_ST7789_PIN_SCLK GPIO_NUM_21


#define LCD_ST7789_X_OFFSET 0
#define LCD_ST7789_Y_OFFSET 0

#define LCD_COLOR_BLACK 0x0000
#define LCD_COLOR_WHITE 0xFFFF
#define LCD_COLOR_RED   0xF800
#define LCD_COLOR_GREEN 0x07E0
#define LCD_COLOR_BLUE  0x001F

esp_err_t lcd_st7789_init(void);
esp_err_t lcd_st7789_init_lvgl(void);
esp_err_t lcd_st7789_show_text(const char *text);
esp_err_t lcd_st7789_start_lvgl_task(void);
void lcd_st7789_set_wifi_connected(bool connected);
void lcd_st7789_set_battery_percent(int percent);
void lcd_st7789_set_status_text(const char *text);
void lcd_st7789_set_main_text(const char *text);
void lcd_st7789_set_bottom_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif
