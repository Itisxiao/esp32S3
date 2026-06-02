idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES led lcd_st7789 esp-wifi-connect nvs_flash esp_netif esp_event esp_hw_support)
