#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WebSocket 连接状态回调
 * @param connected true=已连接, false=已断开
 */
typedef void (*ws_status_cb_t)(bool connected);

/**
 * @brief 收到文本消息回调
 * @param data 消息数据
 * @param len  消息长度
 */
typedef void (*ws_message_cb_t)(const char *data, size_t len);

/**
 * @brief 收到二进制数据回调（如服务端下发的 PCM 音频帧）
 * @param data 二进制数据指针
 * @param len  数据长度（字节）
 */
typedef void (*ws_binary_cb_t)(const uint8_t *data, size_t len);

/**
 * @brief 初始化 WebSocket 客户端
 * @param uri 服务器地址，如 "ws://192.168.1.100:8080/ws"
 * @param status_cb 连接状态回调（可为 NULL）
 * @param msg_cb 文本消息回调（可为 NULL）
 * @param bin_cb 二进制数据回调（可为 NULL）
 * @return ESP_OK 成功
 */
esp_err_t ws_client_init(const char *uri, ws_status_cb_t status_cb,
                         ws_message_cb_t msg_cb, ws_binary_cb_t bin_cb);

/**
 * @brief 启动 WebSocket 连接（非阻塞，后台任务）
 */
esp_err_t ws_client_start(void);

/**
 * @brief 发送文本消息
 * @param data 数据
 * @param len  长度
 * @return ESP_OK 成功
 */
esp_err_t ws_client_send(const char *data, size_t len);

/**
 * @brief 发送二进制消息
 * @param data 数据
 * @param len  长度
 * @return ESP_OK 成功
 */
esp_err_t ws_client_send_bin(const uint8_t *data, size_t len);

/**
 * @brief 获取当前连接状态
 */
bool ws_client_is_connected(void);

/**
 * @brief 断开并销毁 WebSocket 客户端
 */
esp_err_t ws_client_stop(void);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_CLIENT_H
