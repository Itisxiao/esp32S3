#include "websocket_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ws_client";

// 分片重组缓冲区（用于拼接被拆分的 WebSocket 消息）
#define WS_REASSEMBLY_BUF_SIZE 4096
static uint8_t *ws_reassembly_buf = NULL;
static int ws_reassembly_offset = 0;

static esp_websocket_client_handle_t ws_client = NULL;
static bool ws_connected = false;
static ws_status_cb_t user_status_cb = NULL;
static ws_message_cb_t user_message_cb = NULL;
static ws_binary_cb_t user_binary_cb = NULL;

// ================= WebSocket 事件处理 =================

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket 已连接");
            ws_connected = true;
            if (user_status_cb) user_status_cb(true);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket 已断开");
            ws_connected = false;
            if (user_status_cb) user_status_cb(false);
            break;

        case WEBSOCKET_EVENT_DATA: {
            uint8_t op = data->op_code;
            int payload_len = data->payload_len;
            int payload_offset = data->payload_offset;
            int data_len = data->data_len;

            // 文本帧（非分片）：直接分发
            if (op == 0x01) {
                ESP_LOGI(TAG, "收到文本消息, len=%d", data_len);
                if (user_message_cb && data->data_ptr && data_len > 0) {
                    user_message_cb((const char *)data->data_ptr, data_len);
                }
                break;
            }

            // 二进制帧 (0x02) 或续帧 (0x00)：分片重组后分发
            if (op == 0x02 || op == 0x00) {
                ESP_LOGI(TAG, "[WS] 二进制事件: op=0x%02x, data_len=%d, payload_len=%d, offset=%d",
                         op, data_len, payload_len, payload_offset);

                if (payload_len > WS_REASSEMBLY_BUF_SIZE) {
                    ESP_LOGW(TAG, "消息过大 %d > %d, 丢弃", payload_len, WS_REASSEMBLY_BUF_SIZE);
                    ws_reassembly_offset = 0;
                    break;
                }

                // payload_offset==0 表示新消息起始（不能靠 op_code 判断，
                // 因为 esp_websocket_client 在分片读取时每个事件都携带原始 op_code）
                if (payload_offset == 0) {
                    ws_reassembly_offset = 0;
                }

                // 拷贝当前分片到重组缓冲区
                if (data->data_ptr && data_len > 0 &&
                    ws_reassembly_offset + data_len <= WS_REASSEMBLY_BUF_SIZE) {
                    memcpy(ws_reassembly_buf + ws_reassembly_offset,
                           data->data_ptr, data_len);
                    ws_reassembly_offset += data_len;
                }

                // 接收完整个消息，分发
                if (payload_offset + data_len >= payload_len) {
                    ESP_LOGI(TAG, "[WS] 重组完成, 总长度=%d", ws_reassembly_offset);
                    if (user_binary_cb && ws_reassembly_offset > 0) {
                        user_binary_cb(ws_reassembly_buf, ws_reassembly_offset);
                    }
                    ws_reassembly_offset = 0;
                }
            }
            break;
        }

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket 错误");
            ws_connected = false;
            if (user_status_cb) user_status_cb(false);
            break;

        default:
            break;
    }
}

// ================= 公共接口 =================

esp_err_t ws_client_init(const char *uri, ws_status_cb_t status_cb,
                         ws_message_cb_t msg_cb, ws_binary_cb_t bin_cb)
{
    if (uri == NULL) {
        ESP_LOGE(TAG, "URI 不能为空");
        return ESP_ERR_INVALID_ARG;
    }

    if (ws_client != NULL) {
        ESP_LOGW(TAG, "WebSocket 客户端已存在，先停止旧实例");
        ws_client_stop();
    }

    user_status_cb = status_cb;
    user_message_cb = msg_cb;
    user_binary_cb = bin_cb;
    ws_connected = false;

    // 分配分片重组缓冲区
    if (ws_reassembly_buf == NULL) {
        ws_reassembly_buf = malloc(WS_REASSEMBLY_BUF_SIZE);
        if (ws_reassembly_buf == NULL) {
            ESP_LOGE(TAG, "分片重组缓冲区分配失败");
            return ESP_ERR_NO_MEM;
        }
    }
    ws_reassembly_offset = 0;

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,  // 断线自动重连，间隔 5 秒
        .network_timeout_ms = 10000,   // 网络超时 10 秒
        .buffer_size = 4096,           // 增大缓冲区，减少分片
    };

    ws_client = esp_websocket_client_init(&ws_cfg);
    if (ws_client == NULL) {
        ESP_LOGE(TAG, "WebSocket 客户端初始化失败");
        return ESP_FAIL;
    }

    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    ESP_LOGI(TAG, "WebSocket 客户端已初始化, URI=%s", uri);
    return ESP_OK;
}

esp_err_t ws_client_start(void)
{
    if (ws_client == NULL) {
        ESP_LOGE(TAG, "客户端未初始化，请先调用 ws_client_init");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "启动 WebSocket 连接...");
    return esp_websocket_client_start(ws_client);
}

esp_err_t ws_client_send(const char *data, size_t len)
{
    if (ws_client == NULL || !ws_connected) {
        ESP_LOGW(TAG, "WebSocket 未连接，无法发送");
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_text(ws_client, data, len, portMAX_DELAY);
    if (sent < 0) {
        ESP_LOGE(TAG, "发送失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "已发送 %d 字节", sent);
    return ESP_OK;
}

esp_err_t ws_client_send_bin(const uint8_t *data, size_t len)
{
    if (ws_client == NULL || !ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_bin(ws_client, (const char *)data, (int)len, portMAX_DELAY);
    if (sent < 0) {
        ESP_LOGE(TAG, "二进制发送失败: %d", sent);
        return ESP_FAIL;
    }
    if ((size_t)sent != len) {
        ESP_LOGW(TAG, "二进制发送不完整: %d/%d", sent, (int)len);
    }

    return ESP_OK;
}

bool ws_client_is_connected(void)
{
    return ws_connected;
}

esp_err_t ws_client_stop(void)
{
    if (ws_client == NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "停止 WebSocket 客户端...");
    ws_connected = false;

    esp_err_t ret = esp_websocket_client_stop(ws_client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "停止客户端失败: %s", esp_err_to_name(ret));
    }

    ret = esp_websocket_client_destroy(ws_client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "销毁客户端失败: %s", esp_err_to_name(ret));
    }

    ws_client = NULL;
    user_status_cb = NULL;
    user_message_cb = NULL;
    user_binary_cb = NULL;

    // 释放分片重组缓冲区
    if (ws_reassembly_buf) {
        free(ws_reassembly_buf);
        ws_reassembly_buf = NULL;
    }
    ws_reassembly_offset = 0;

    ESP_LOGI(TAG, "WebSocket 客户端已停止");
    return ESP_OK;
}
