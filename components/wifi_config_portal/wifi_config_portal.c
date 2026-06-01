#include "wifi_config_portal.h"
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"

#include "lwip/ip_addr.h"

static const char *TAG = "wifi_portal";




// ================= 内部状态 =================
static httpd_handle_t server = NULL;
static bool is_connected = false;
static int retry_count = 0;
#define MAX_RETRY 5

// ================= HTML 页面 =================
static const char *INDEX_HTML = 
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:sans-serif;text-align:center;padding:20px;} input{padding:10px;margin:5px;width:80%;} button{padding:10px 20px;background:#007bff;color:white;border:none;border-radius:5px;}</style>"
    "</head><body><h2>WiFi 配置</h2><form action='/save' method='POST'>"
    "<input type='text' name='ssid' placeholder='WiFi 名称 (SSID)' required><br>"
    "<input type='password' name='pass' placeholder='WiFi 密码'><br>"
    "<button type='submit'>保存并连接</button></form></body></html>";

static const char *SUCCESS_HTML = 
    "<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body><h2>已保存!</h2><p>正在尝试连接... 请查看屏幕状态。</p>"
    "<a href='/'>返回</a></body></html>";

// ================= HTTP 处理函数 =================

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[512];
    int ret, total_len = req->content_len;
    int cur_len = 0;
    int timeout = 0;
    
    if (total_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    while (cur_len < total_len && timeout < 100) {
        ret = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (ret > 0) {
            cur_len += ret;
            timeout = 0;
        } else if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            timeout++;
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            ESP_LOGE(TAG, "httpd_req_recv failed: %d", ret);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
    }
    buf[cur_len] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    
    char *ssid_start = strstr(buf, "ssid=");
    if (ssid_start) {
        ssid_start += 5;
        char *end = strchr(ssid_start, '&');
        if (end) {
            strncpy(ssid, ssid_start, end - ssid_start);
        } else {
            strcpy(ssid, ssid_start);
        }
    }

    char *pass_start = strstr(buf, "pass=");
    if (pass_start) {
        pass_start += 5;
        char *end = strchr(pass_start, '&');
        if (end) {
            strncpy(pass, pass_start, end - pass_start);
        } else {
            strcpy(pass, pass_start);
        }
        size_t len = strlen(pass);
        while (len > 0 && (pass[len-1] == '\r' || pass[len-1] == '\n' || pass[len-1] == ' ')) {
            pass[--len] = 0;
        }
    }

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Received Config: SSID=%s, PASS=%s", ssid, pass);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "pass", pass);
        nvs_commit(nvs);
        nvs_close(nvs);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    }

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password) - 1);
    
    if (strlen(pass) == 0) {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA config: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi config failed");
        return ESP_FAIL;
    }
    
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect WiFi: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi connect failed");
        return ESP_FAIL;
    }
    
    retry_count = 0;
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, SUCCESS_HTML, strlen(SUCCESS_HTML));
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"connected\":%s}", is_connected ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

static esp_err_t generate_204_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, NULL, 0);
}

static const httpd_uri_t uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
};

static const httpd_uri_t uri_save = {
    .uri       = "/save",
    .method    = HTTP_POST,
    .handler   = save_post_handler,
};

static const httpd_uri_t uri_status = {
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_get_handler,
};

static const httpd_uri_t uri_generate_204 = {
    .uri       = "/generate_204",
    .method    = HTTP_GET,
    .handler   = generate_204_handler,
};

static void start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_save);
        httpd_register_uri_handler(server, &uri_status);
        httpd_register_uri_handler(server, &uri_generate_204);
        ESP_LOGI(TAG, "HTTP Server started");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP Server");
    }
}

// ================= WiFi 事件处理 =================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_DISCONNECTED: {
                is_connected = false;
                wifi_event_sta_disconnected_t* disconnect_event = 
                    (wifi_event_sta_disconnected_t*) event_data;
                ESP_LOGW(TAG, "Disconnected from AP, reason: %d", 
                         disconnect_event->reason);
                if (retry_count < MAX_RETRY) {
                    retry_count++;
                    ESP_LOGI(TAG, "Retrying connection (%d/%d)...", 
                             retry_count, MAX_RETRY);
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "Max retries reached. Keeping AP open.");
                }
                break;
            }
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started");
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGW(TAG, "AP stopped");
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* connect_event = 
                    (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", 
                         MAC2STR(connect_event->mac), connect_event->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* disconnect_event = 
                    (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", 
                         MAC2STR(disconnect_event->mac), disconnect_event->aid);
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        is_connected = true;
        retry_count = 0;
    }
}

// ================= 设置AP静态IP =================
static void set_ap_static_ip(void) {
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to get AP netif handle");
        return;
    }
    
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr = ipaddr_addr(WIFI_CONFIG_PORTAL_AP_IP);
    ip_info.gw.addr = ipaddr_addr(WIFI_CONFIG_PORTAL_AP_IP);
    ip_info.netmask.addr = ipaddr_addr("255.255.255.0");
    
    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Failed to stop DHCP server: %s", esp_err_to_name(err));
        return;
    }
    
    err = esp_netif_set_ip_info(ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP IP info: %s", esp_err_to_name(err));
        return;
    }
    
    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start DHCP server: %s", esp_err_to_name(err));
    }
    
    ESP_LOGI(TAG, "AP IP set to: %s", WIFI_CONFIG_PORTAL_AP_IP);
}

// ================= 异步启动任务 =================

static void wifi_start_async_task(void *pvParameters) {
    ESP_LOGI(TAG, "[Async Task] Starting WiFi Portal...");
    
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "[Async Task] Step 1: Setting Mode to AP");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    
    vTaskDelay(pdMS_TO_TICKS(300));
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = PORTAL_AP_SSID,
            .ssid_len = strlen(PORTAL_AP_SSID),
            .password = PORTAL_AP_PASS,
            .channel = PORTAL_AP_CHANNEL,
            .max_connection = PORTAL_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_LOGI(TAG, "[Async Task] Step 2: Setting AP Config");
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(err));
    }
    
    ESP_LOGI(TAG, "[Async Task] Step 2.5: Setting AP Static IP");
    set_ap_static_ip();
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "[Async Task] Step 3: Starting WiFi");
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "[Async Task] Step 4: WiFi Started");
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "[Async Task] Step 5: Starting HTTP Server");
    start_http_server();
    
    ESP_LOGI(TAG, "[Async Task] Step 6: Portal Ready!");
    
    vTaskDelete(NULL);
}


// ================= 公共接口 =================

esp_err_t wifi_portal_init(void) {
    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化网络接口和事件循环
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. 创建默认 AP 接口 (必须在 esp_wifi_init 之前)
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta(); // 如果后续需要连路由器

    // 4. 初始化 WiFi 驱动
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 5. 注册事件处理器
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // 6. ✅ 关键：创建异步任务去执行耗时的 set_mode 和 start
    // 优先级设为 1 (低)，避免阻塞高优先级的 LVGL 或主任务
    xTaskCreate(wifi_start_async_task, "wifi_start", 4096, NULL, 1, NULL);

    return ESP_OK;
}

bool wifi_portal_is_connected(void) {
    return is_connected;
}