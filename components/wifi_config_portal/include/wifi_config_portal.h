#ifndef WIFI_CONFIG_PORTAL_H
#define WIFI_CONFIG_PORTAL_H

#include <stdbool.h>

#include "esp_err.h"

#define PORTAL_AP_SSID "ESP32S3-Setup"
#define PORTAL_AP_PASS  "12345678"
#define WIFI_CONFIG_PORTAL_AP_IP "192.168.4.1"
#define PORTAL_AP_CHANNEL   1
#define PORTAL_AP_MAX_CONN  4

/**
 * @brief 初始化 WiFi 配网门户
 * @note 此函数是非阻塞的，它会启动一个后台任务来完成 WiFi 启动
 */
esp_err_t wifi_portal_init(void);

/**
 * @brief 获取当前 WiFi 连接状态
 * @return true 如果已连接到路由器，否则 false
 */
bool wifi_portal_is_connected(void);

#endif
