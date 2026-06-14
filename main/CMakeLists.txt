

idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "." "assets"
                    REQUIRES led lcd_st7789 max98357a esp-wifi-connect websocket_client nvs_flash esp_netif esp_event esp_hw_support
                    )




