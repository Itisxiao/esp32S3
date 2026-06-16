#ifndef VAD_H
#define VAD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===== 状态枚举 =====
typedef enum {
    VAD_STATE_IDLE = 0,
    VAD_STATE_LISTENING,
    VAD_STATE_ENDING,
} vad_state_t;

// ===== 事件枚举 (回调通知上层) =====
typedef enum {
    VAD_EVENT_START,    // 检测到语音，应开始录音
    VAD_EVENT_STOP,     // 确认静音，应停止录音
} vad_event_t;

// ===== 事件回调 =====
// 注意：此回调在 stream_task 上下文中调用，应仅设置标志或发送信号，不做阻塞操作
typedef void (*vad_event_cb_t)(vad_event_t event, int volume, void *user_ctx);

// ===== 配置结构体 =====
typedef struct {
    int      voice_threshold;    // 有声阈值, 0 则用默认 1000
    int      silence_threshold;  // 静音阈值, 0 则用默认 500
    int      start_frames;       // 连续 N 帧有声触发 start, 0 则用默认 3
    int      silence_frames;     // 连续 M 帧静音触发 ending, 0 则用默认 25
    uint32_t ending_timeout_ms;  // ENDING 超时, 0 则用默认 300
    uint32_t min_record_ms;      // 最短录音, 0 则用默认 500
    uint32_t max_record_ms;      // 最长录音, 0 则用默认 30000
    uint32_t cooldown_ms;        // 冷却期, 0 则用默认 500
} vad_config_t;

// ===== API =====

/**
 * @brief 初始化 VAD
 * @param config 配置，传 NULL 使用默认值
 * @param cb     事件回调
 * @param user_ctx 用户上下文
 */
void vad_init(const vad_config_t *config, vad_event_cb_t cb, void *user_ctx);

/**
 * @brief 处理一帧音频数据（在 stream callback 中调用）
 * @param samples     16bit PCM 采样点
 * @param frame_count 采样点数量
 */
void vad_process(const int16_t *samples, size_t frame_count);

/**
 * @brief 获取当前 VAD 状态
 */
vad_state_t vad_get_state(void);

/**
 * @brief 获取最近一帧的 RMS 音量
 */
int vad_get_volume(void);

/**
 * @brief 是否正在录音（LISTENING 或 ENDING）
 */
bool vad_is_recording(void);

/**
 * @brief 强制停止录音（WS 断开或按键干预时调用）
 */
void vad_force_stop(void);

/**
 * @brief 暂停/恢复 VAD 检测
 */
void vad_set_enabled(bool enabled);

/**
 * @brief 获取统计信息
 */
void vad_get_stats(uint32_t *total_recordings, uint32_t *total_chunks);

#ifdef __cplusplus
}
#endif

#endif // VAD_H
