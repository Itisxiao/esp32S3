idf_component_register(SRCS "wifi_config_portal.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_server esp_netif esp_wifi nvs_flash)
