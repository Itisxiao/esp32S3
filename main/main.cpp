#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lcd_st7789.h"
#include "max98357a.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include <cstdio>

static const char *TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== Application Start ===");

    ESP_LOGI(TAG, "Step 1: Init NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "Step 1: NVS OK");

    ESP_LOGI(TAG, "Step 2: Init Netif...");
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(ret));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Step 2: Netif OK");

    ESP_LOGI(TAG, "Step 3: Init Event Loop...");
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop init failed: %s", esp_err_to_name(ret));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Step 3: Event Loop OK");

    ESP_LOGI(TAG, "Step 4: Init Speaker...");
    bool speaker_ok = (max98357a_init() == ESP_OK);
    if (speaker_ok) {
        max98357a_play_tone(1000, 120, 3000);
    }
    ESP_LOGI(TAG, "Step 4: Speaker %s", speaker_ok ? "OK" : "FAILED");

    ESP_LOGI(TAG, "Step 5: Init LCD...");
    bool lcd_ok = (lcd_st7789_init() == ESP_OK);
    if (lcd_ok) {
        lcd_ok = (lcd_st7789_init_lvgl() == ESP_OK);
    }
    if (lcd_ok) {
        lcd_ok = (lcd_st7789_start_lvgl_task() == ESP_OK);
    }
    ESP_LOGI(TAG, "Step 5: LCD %s, internal RAM: %d bytes",
             lcd_ok ? "OK" : "FAILED",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (lcd_ok) {
        lcd_st7789_show_text("Booting");
    }

    ESP_LOGI(TAG, "Step 6: Init Wi-Fi Manager...");
    auto& wifi_manager = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.ssid_prefix = "Chenjinxiao";
    config.language = "zh-CN";

    bool wifi_ok = wifi_manager.Initialize(config);
    ESP_LOGI(TAG, "Step 6: Wi-Fi Manager: %s", wifi_ok ? "OK" : "FAIL");
    if (!wifi_ok) while(1) vTaskDelay(pdMS_TO_TICKS(1000));

    auto& ssid_list = SsidManager::GetInstance().GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGI(TAG, "Step 7: Start Config AP");
        wifi_manager.StartConfigAp();
        if (lcd_ok) lcd_st7789_show_text("AP Mode");
    } else {
        ESP_LOGI(TAG, "Step 7: Start Station");
        wifi_manager.StartStation();
        if (lcd_ok) lcd_st7789_show_text("Connecting");
    }

    int timeout_counter = 0;
    bool is_online = false;
    while (1) {
        if (wifi_manager.IsConnected()) {
            if (!is_online) {
                ESP_LOGI(TAG, "Wi-Fi Connected!");
                if (lcd_ok) {
                    lcd_st7789_set_status_text("ONLINE");
                    lcd_st7789_show_text("Connected");
                    lcd_st7789_set_battery_percent(80);
                }
                is_online = true;
            }
            timeout_counter = 0;
        } else {
            is_online = false;
            if (!wifi_manager.IsConfigMode()) {
                timeout_counter++;
                if (timeout_counter > 60) {
                    ESP_LOGW(TAG, "Timeout -> Switch AP");
                    if (lcd_ok) lcd_st7789_show_text("Fail->AP");
                    wifi_manager.StopStation();
                    wifi_manager.StartConfigAp();
                    timeout_counter = 0;
                } else if (timeout_counter % 10 == 0 && lcd_ok) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "Try %ds", timeout_counter);
                    lcd_st7789_show_text(buf);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
