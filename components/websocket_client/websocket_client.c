#include "websocket_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ws_client";

static esp_websocket_client_handle_t ws_client = NULL;
static bool ws_connected = false;
static ws_status_cb_t user_status_cb = NULL;
static ws_message_cb_t user_message_cb = NULL;

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

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01 || data->op_code == 0x02) {
                // 文本帧(0x01) 或 二进制帧(0x02)
                ESP_LOGI(TAG, "收到消息, len=%d, op_code=0x%02x", data->data_len, data->op_code);
                if (user_message_cb && data->data_ptr && data->data_len > 0) {
                    user_message_cb((const char *)data->data_ptr, data->data_len);
                }
            }
            break;

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

esp_err_t ws_client_init(const char *uri, ws_status_cb_t status_cb, ws_message_cb_t msg_cb)
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
    ws_connected = false;

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,  // 断线自动重连，间隔 5 秒
        .network_timeout_ms = 10000,   // 网络超时 10 秒
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

    ESP_LOGI(TAG, "WebSocket 客户端已停止");
    return ESP_OK;
}
