idf_component_register(SRCS "lcd_st7789.c" "font_chinese_16.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_lcd esp_timer lvgl)
