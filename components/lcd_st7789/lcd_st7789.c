#include "lcd_st7789.h"

#include <assert.h>
#include <stdlib.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#define LCD_HOST SPI3_HOST
#define LCD_SPI_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LVGL_TICK_PERIOD_MS 2
#define LVGL_BUFFER_LINES 10

static const char *TAG = "lcd_st7789";
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_main_label;
static lv_obj_t *s_bottom_label;

static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_lvgl_mutex = NULL;
static bool s_lvgl_initialized = false;
static bool s_lvgl_task_started = false;

static void lvgl_lock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    }
}

static void lvgl_unlock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreGiveRecursive(s_lvgl_mutex);
    }
}

static bool lcd_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                            esp_lcd_panel_io_event_data_t *edata,
                            void *user_ctx)
{
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_drv);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        lvgl_lock();
        lv_timer_handler();
        lvgl_unlock();
    }
}

esp_err_t lcd_st7789_init(void)
{
    ESP_LOGI(TAG, "LCD init step 1: Configuring backlight GPIO");
    gpio_config_t backlight_gpio = {
        .pin_bit_mask = 1ULL << LCD_ST7789_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight_gpio), TAG, "config backlight gpio failed");
    gpio_set_level(LCD_ST7789_PIN_BL, 0);
    ESP_LOGI(TAG, "LCD init step 2: Initializing SPI bus");

    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_ST7789_PIN_SCLK,
        .mosi_io_num = LCD_ST7789_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_ST7789_WIDTH * LVGL_BUFFER_LINES * sizeof(lv_color_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_DISABLED), TAG, "init spi bus failed");
    ESP_LOGI(TAG, "LCD init step 3: Creating panel IO");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_ST7789_PIN_DC,
        .cs_gpio_num = LCD_ST7789_PIN_CS,
        .pclk_hz = LCD_SPI_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_flush_ready,
        .user_ctx = &s_disp_drv,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle),
                        TAG, "create panel io failed");
    ESP_LOGI(TAG, "LCD init step 4: Creating ST7789 panel");

    s_panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_ST7789_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel),
                        TAG, "create st7789 panel failed");
    ESP_LOGI(TAG, "LCD init step 5: Resetting panel");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset panel failed");
    ESP_LOGI(TAG, "LCD init step 6: Initializing panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init panel failed");
    ESP_LOGI(TAG, "LCD init step 7: Setting panel configs");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert color failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "turn on display failed");
    gpio_set_level(LCD_ST7789_PIN_BL, 1);
    ESP_LOGI(TAG, "LCD init step 8: Hardware init completed");

    return ESP_OK;
}

esp_err_t lcd_st7789_init_lvgl(void)
{
    if (s_lvgl_initialized) {
        return ESP_OK;
    }
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "LCD init step 9: Initializing LVGL");
    lv_init();
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_lvgl_mutex) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LCD init step 10: Allocating LVGL buffers");

    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(LCD_ST7789_WIDTH * LVGL_BUFFER_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(LCD_ST7789_WIDTH * LVGL_BUFFER_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "buf1=%p, buf2=%p", buf1, buf2);

    if (!buf1 || !buf2) {
        free(buf1);
        free(buf2);
        ESP_LOGE(TAG, "Cannot allocate internal DMA memory for LVGL");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LCD init step 11: Setting up LVGL display");

    lvgl_lock();
    lv_disp_draw_buf_init(&s_disp_buf, buf1, buf2, LCD_ST7789_WIDTH * LVGL_BUFFER_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_ST7789_WIDTH;
    s_disp_drv.ver_res = LCD_ST7789_HEIGHT;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_disp_buf;
    s_disp_drv.user_data = s_panel;
    lv_disp_drv_register(&s_disp_drv);
    lvgl_unlock();
    ESP_LOGI(TAG, "LCD init step 12: Creating LVGL timer");

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG, "create lvgl tick failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "start lvgl tick failed");
    s_lvgl_initialized = true;
    ESP_LOGI(TAG, "LVGL init completed successfully!");

    return ESP_OK;
}

esp_err_t lcd_st7789_start_lvgl_task(void)
{
    if (!s_lvgl_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lvgl_task_started) {
        return ESP_OK;
    }
    BaseType_t task_created = xTaskCreate(lvgl_task, "lvgl", 4096, NULL, 0, NULL);
    if (task_created != pdPASS) {
        return ESP_FAIL;
    }
    s_lvgl_task_started = true;
    return ESP_OK;
}

esp_err_t lcd_st7789_show_text(const char *text)
{
    if (!s_lvgl_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_lock();
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    lv_obj_t *status_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(status_bar);
    lv_obj_set_size(status_bar, LCD_ST7789_WIDTH, 28);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);

    s_wifi_label = lv_label_create(status_bar);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_white(), 0);
    lv_obj_align(s_wifi_label, LV_ALIGN_LEFT_MID, 8, 0);

    s_status_label = lv_label_create(status_bar);
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);

    s_battery_label = lv_label_create(status_bar);
    lv_obj_set_style_text_color(s_battery_label, lv_color_white(), 0);
    lv_obj_align(s_battery_label, LV_ALIGN_RIGHT_MID, -8, 0);

    s_main_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_main_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_main_label, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_main_label, LCD_ST7789_WIDTH);
    lv_obj_set_style_text_align(s_main_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_main_label, LV_ALIGN_CENTER, 0, -12);

    s_bottom_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_bottom_label, lv_color_hex(0xA7F3D0), 0);
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(s_bottom_label, &lv_font_simsun_16_cjk, 0);
#else
    lv_obj_set_style_text_font(s_bottom_label, &lv_font_montserrat_20, 0);
#endif
    lv_obj_set_width(s_bottom_label, LCD_ST7789_WIDTH);
    lv_obj_set_style_text_align(s_bottom_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_bottom_label, LV_ALIGN_BOTTOM_MID, 0, -24);

    lcd_st7789_set_wifi_connected(true);
    lcd_st7789_set_battery_percent(100);
    lcd_st7789_set_status_text("ST7789");
    lcd_st7789_set_main_text(text);
    lcd_st7789_set_bottom_text("你好，LVGL");
    lv_timer_handler();
    lvgl_unlock();
    return ESP_OK;
}

void lcd_st7789_set_wifi_connected(bool connected)
{
    if (!s_lvgl_initialized || !s_wifi_label) {
        return;
    }
    lvgl_lock();
    lv_label_set_text(s_wifi_label, connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);
    lvgl_unlock();
}

void lcd_st7789_set_battery_percent(int percent)
{
    if (!s_lvgl_initialized || !s_battery_label) {
        return;
    }
    lvgl_lock();
    if (percent >= 80) {
        lv_label_set_text(s_battery_label, LV_SYMBOL_BATTERY_FULL);
    } else if (percent >= 40) {
        lv_label_set_text(s_battery_label, LV_SYMBOL_BATTERY_2);
    } else if (percent >= 15) {
        lv_label_set_text(s_battery_label, LV_SYMBOL_BATTERY_1);
    } else {
        lv_label_set_text(s_battery_label, LV_SYMBOL_BATTERY_EMPTY);
    }
    lvgl_unlock();
}

void lcd_st7789_set_status_text(const char *text)
{
    if (s_lvgl_initialized && s_status_label) {
        lvgl_lock();
        lv_label_set_text(s_status_label, text);
        lvgl_unlock();
    }
}

void lcd_st7789_set_main_text(const char *text)
{
    if (s_lvgl_initialized && s_main_label) {
        lvgl_lock();
        lv_label_set_text(s_main_label, text);
        lvgl_unlock();
    }
}

void lcd_st7789_set_bottom_text(const char *text)
{
    if (s_lvgl_initialized && s_bottom_label) {
        lvgl_lock();
        lv_label_set_text(s_bottom_label, text);
        lvgl_unlock();
    }
}
