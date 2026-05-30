#include "lcd_st7789.h"

#include <ctype.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_HOST SPI2_HOST
#define LCD_SPI_CLOCK_HZ (40 * 1000 * 1000)

static const char *TAG = "lcd_st7789";
static spi_device_handle_t s_lcd_spi;

static esp_err_t lcd_write(const uint8_t *data, int len, int dc_level)
{
    if (len == 0) {
        return ESP_OK;
    }

    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = data,
    };
    gpio_set_level(LCD_ST7789_PIN_DC, dc_level);
    return spi_device_polling_transmit(s_lcd_spi, &trans);
}

static esp_err_t lcd_cmd(uint8_t cmd)
{
    return lcd_write(&cmd, 1, 0);
}

static esp_err_t lcd_data(const uint8_t *data, int len)
{
    return lcd_write(data, len, 1);
}

static esp_err_t lcd_cmd_data(uint8_t cmd, const uint8_t *data, int len)
{
    ESP_RETURN_ON_ERROR(lcd_cmd(cmd), TAG, "write command failed");
    return lcd_data(data, len);
}

static esp_err_t lcd_set_window(int x0, int y0, int x1, int y1)
{
    x0 += LCD_ST7789_X_OFFSET;
    x1 += LCD_ST7789_X_OFFSET;
    y0 += LCD_ST7789_Y_OFFSET;
    y1 += LCD_ST7789_Y_OFFSET;

    uint8_t caset[] = {
        (uint8_t)(x0 >> 8), (uint8_t)x0,
        (uint8_t)(x1 >> 8), (uint8_t)x1,
    };
    uint8_t raset[] = {
        (uint8_t)(y0 >> 8), (uint8_t)y0,
        (uint8_t)(y1 >> 8), (uint8_t)y1,
    };

    ESP_RETURN_ON_ERROR(lcd_cmd_data(0x2A, caset, sizeof(caset)), TAG, "set column failed");
    ESP_RETURN_ON_ERROR(lcd_cmd_data(0x2B, raset, sizeof(raset)), TAG, "set row failed");
    return lcd_cmd(0x2C);
}

static esp_err_t lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x >= LCD_ST7789_WIDTH || y >= LCD_ST7789_HEIGHT || w <= 0 || h <= 0) {
        return ESP_OK;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > LCD_ST7789_WIDTH) {
        w = LCD_ST7789_WIDTH - x;
    }
    if (y + h > LCD_ST7789_HEIGHT) {
        h = LCD_ST7789_HEIGHT - y;
    }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(lcd_set_window(x, y, x + w - 1, y + h - 1), TAG, "set window failed");

    uint8_t line[LCD_ST7789_WIDTH * 2];
    for (int i = 0; i < w; i++) {
        line[i * 2] = color >> 8;
        line[i * 2 + 1] = color & 0xFF;
    }

    for (int row = 0; row < h; row++) {
        ESP_RETURN_ON_ERROR(lcd_data(line, w * 2), TAG, "fill row failed");
    }
    return ESP_OK;
}

static void font5x7(char c, uint8_t cols[5])
{
    memset(cols, 0, 5);
    switch ((char)tolower((unsigned char)c)) {
    case 'd': { const uint8_t v[5] = {0x38, 0x44, 0x44, 0x48, 0x7F}; memcpy(cols, v, 5); break; }
    case 'e': { const uint8_t v[5] = {0x38, 0x54, 0x54, 0x54, 0x18}; memcpy(cols, v, 5); break; }
    case 'h': { const uint8_t v[5] = {0x7F, 0x08, 0x04, 0x04, 0x78}; memcpy(cols, v, 5); break; }
    case 'l': { const uint8_t v[5] = {0x00, 0x41, 0x7F, 0x40, 0x00}; memcpy(cols, v, 5); break; }
    case 'o': { const uint8_t v[5] = {0x38, 0x44, 0x44, 0x44, 0x38}; memcpy(cols, v, 5); break; }
    case 'r': { const uint8_t v[5] = {0x7C, 0x08, 0x04, 0x04, 0x08}; memcpy(cols, v, 5); break; }
    case 'w': { const uint8_t v[5] = {0x1F, 0x20, 0x18, 0x20, 0x1F}; memcpy(cols, v, 5); break; }
    default:
        break;
    }
}

static esp_err_t lcd_draw_char(int x, int y, char c, uint16_t color, uint16_t bg_color, int scale)
{
    uint8_t cols[5];
    font5x7(c, cols);

    for (int col = 0; col < 6; col++) {
        uint8_t bits = col < 5 ? cols[col] : 0;
        for (int row = 0; row < 8; row++) {
            uint16_t pixel_color = (bits & (1U << row)) ? color : bg_color;
            ESP_RETURN_ON_ERROR(lcd_fill_rect(x + col * scale, y + row * scale, scale, scale, pixel_color),
                                TAG, "draw char failed");
        }
    }
    return ESP_OK;
}

esp_err_t lcd_st7789_init(void)
{
    gpio_config_t ctrl_io = {
        .pin_bit_mask = (1ULL << LCD_ST7789_PIN_DC) |
                        (1ULL << LCD_ST7789_PIN_RST) |
                        (1ULL << LCD_ST7789_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ctrl_io), TAG, "config gpio failed");

    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_ST7789_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_ST7789_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_ST7789_WIDTH * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "init spi bus failed");

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = LCD_ST7789_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &devcfg, &s_lcd_spi), TAG, "add spi device failed");

    gpio_set_level(LCD_ST7789_PIN_BL, 1);
    gpio_set_level(LCD_ST7789_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(LCD_ST7789_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(lcd_cmd(0x01), TAG, "software reset failed");
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_RETURN_ON_ERROR(lcd_cmd(0x11), TAG, "sleep out failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t colmod[] = {0x55};
    const uint8_t madctl[] = {0x00};
    ESP_RETURN_ON_ERROR(lcd_cmd_data(0x3A, colmod, sizeof(colmod)), TAG, "set color mode failed");
    ESP_RETURN_ON_ERROR(lcd_cmd_data(0x36, madctl, sizeof(madctl)), TAG, "set memory access failed");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x21), TAG, "invert display failed");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x29), TAG, "display on failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    return lcd_st7789_fill_screen(LCD_COLOR_BLACK);
}

esp_err_t lcd_st7789_fill_screen(uint16_t color)
{
    return lcd_fill_rect(0, 0, LCD_ST7789_WIDTH, LCD_ST7789_HEIGHT, color);
}

esp_err_t lcd_st7789_draw_string(int x, int y, const char *text, uint16_t color, uint16_t bg_color, int scale)
{
    if (scale < 1) {
        scale = 1;
    }

    for (int i = 0; text[i] != '\0'; i++) {
        ESP_RETURN_ON_ERROR(lcd_draw_char(x + i * 6 * scale, y, text[i], color, bg_color, scale),
                            TAG, "draw string failed");
    }
    return ESP_OK;
}
