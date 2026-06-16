#include "vad.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "vad";

// ===== 默认参数 =====
#define DEFAULT_VOICE_THRESHOLD    2000    // 有声阈值 (大幅提高, 仅大声触发)
#define DEFAULT_SILENCE_THRESHOLD  250     // 静音阈值 (降低, 更容易检测静音)
#define DEFAULT_START_FRAMES       8       // 160ms @20ms/chunk, 更长确认时间
#define DEFAULT_SILENCE_FRAMES     35      // 700ms
#define DEFAULT_ENDING_TIMEOUT_MS  350
#define DEFAULT_MIN_RECORD_MS      800
#define DEFAULT_MAX_RECORD_MS      30000
#define DEFAULT_COOLDOWN_MS        500

// ===== 内部状态 =====
static vad_state_t    s_state             = VAD_STATE_IDLE;
static bool           s_enabled           = true;
static vad_event_cb_t s_event_cb          = NULL;
static void          *s_user_ctx          = NULL;

// 配置（填充默认值）
static int      s_voice_threshold   = DEFAULT_VOICE_THRESHOLD;
static int      s_silence_threshold = DEFAULT_SILENCE_THRESHOLD;
static int      s_start_frames      = DEFAULT_START_FRAMES;
static int      s_silence_frames    = DEFAULT_SILENCE_FRAMES;
static uint32_t s_ending_timeout_ms = DEFAULT_ENDING_TIMEOUT_MS;
static uint32_t s_min_record_ms     = DEFAULT_MIN_RECORD_MS;
static uint32_t s_max_record_ms     = DEFAULT_MAX_RECORD_MS;
static uint32_t s_cooldown_ms       = DEFAULT_COOLDOWN_MS;

// 计数器
static int      s_voice_count       = 0;   // 连续有声帧
static int      s_silence_count     = 0;   // 连续静音帧
static int      s_last_volume       = 0;   // 最近一帧 RMS

// 时间戳 (ms)
static int64_t  s_state_enter_time  = 0;   // 进入当前状态的时间
static int64_t  s_record_start_time = 0;   // 录音开始时间
static int64_t  s_last_stop_time    = 0;   // 上次停止时间（用于冷却）

// 统计
static uint32_t s_total_recordings  = 0;
static uint32_t s_total_chunks      = 0;

// ===== 辅助函数 =====

static int64_t get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int calc_rms(const int16_t *samples, size_t count)
{
    if (count == 0) return 0;
    int64_t sum_sq = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        sum_sq += s * s;
    }
    return (int)sqrtf((float)(sum_sq / count));
}

// ===== 公共接口 =====

void vad_init(const vad_config_t *config, vad_event_cb_t cb, void *user_ctx)
{
    s_event_cb = cb;
    s_user_ctx = user_ctx;

    if (config) {
        if (config->voice_threshold > 0)    s_voice_threshold   = config->voice_threshold;
        if (config->silence_threshold > 0)  s_silence_threshold = config->silence_threshold;
        if (config->start_frames > 0)       s_start_frames      = config->start_frames;
        if (config->silence_frames > 0)     s_silence_frames    = config->silence_frames;
        if (config->ending_timeout_ms > 0)  s_ending_timeout_ms = config->ending_timeout_ms;
        if (config->min_record_ms > 0)      s_min_record_ms     = config->min_record_ms;
        if (config->max_record_ms > 0)      s_max_record_ms     = config->max_record_ms;
        if (config->cooldown_ms > 0)        s_cooldown_ms       = config->cooldown_ms;
    }

    s_state         = VAD_STATE_IDLE;
    s_enabled       = true;
    s_voice_count   = 0;
    s_silence_count = 0;
    s_last_volume   = 0;

    ESP_LOGI(TAG, "VAD 初始化完成: voice_th=%d, silence_th=%d, start=%d帧, silence=%d帧",
             s_voice_threshold, s_silence_threshold, s_start_frames, s_silence_frames);
}

void vad_process(const int16_t *samples, size_t frame_count)
{
    if (!s_enabled || samples == NULL || frame_count == 0) {
        return;
    }

    int rms = calc_rms(samples, frame_count);
    s_last_volume = rms;

    int64_t now = get_time_ms();
    bool has_voice = (rms > s_voice_threshold);

    switch (s_state) {
        case VAD_STATE_IDLE: {
            // 连续有声帧计数
            if (has_voice) {
                s_voice_count++;
            } else {
                s_voice_count = 0;
            }

            // 触发条件：连续 N 帧有声 + 冷却期已过
            if (s_voice_count >= s_start_frames) {
                int64_t since_stop = now - s_last_stop_time;
                if (s_last_stop_time == 0 || since_stop >= (int64_t)s_cooldown_ms) {
                    // 开始录音
                    s_state = VAD_STATE_LISTENING;
                    s_record_start_time = now;
                    s_state_enter_time = now;
                    s_silence_count = 0;
                    s_voice_count = 0;
                    s_total_recordings++;

                    ESP_LOGI(TAG, "VAD START: rms=%d, recording #%lu",
                             rms, (unsigned long)s_total_recordings);

                    if (s_event_cb) {
                        s_event_cb(VAD_EVENT_START, rms, s_user_ctx);
                    }
                }
            }
            break;
        }

        case VAD_STATE_LISTENING: {
            s_total_chunks++;

            // 静音计数
            if (rms < s_silence_threshold) {
                s_silence_count++;
            } else {
                s_silence_count = 0;
            }

            int64_t record_duration = now - s_record_start_time;

            // 超时强制停止
            if (record_duration >= (int64_t)s_max_record_ms) {
                ESP_LOGW(TAG, "VAD: 录音超时 %lldms，强制停止", (long long)record_duration);
                s_state = VAD_STATE_IDLE;
                s_voice_count = 0;
                s_silence_count = 0;
                s_last_stop_time = now;

                if (s_event_cb) {
                    s_event_cb(VAD_EVENT_STOP, rms, s_user_ctx);
                }
            }
            // 连续静音 → 进入 ENDING（需满足最短录音时间）
            else if (s_silence_count >= s_silence_frames) {
                if (record_duration >= (int64_t)s_min_record_ms) {
                    s_state = VAD_STATE_ENDING;
                    s_state_enter_time = now;
                    ESP_LOGI(TAG, "VAD ENDING: rms=%d, duration=%lldms",
                             rms, (long long)record_duration);
                }
                // 录音太短则继续等（不重置静音计数，让其自然过渡）
            }
            break;
        }

        case VAD_STATE_ENDING: {
            s_total_chunks++;
            int64_t ending_duration = now - s_state_enter_time;

            // 语音恢复 → 回到 LISTENING
            if (has_voice) {
                s_state = VAD_STATE_LISTENING;
                s_silence_count = 0;
                ESP_LOGI(TAG, "VAD: 语音恢复，回到 LISTENING, rms=%d", rms);
            }
            // ENDING 超时 → 确认停止
            else if (ending_duration >= (int64_t)s_ending_timeout_ms) {
                int64_t record_duration = now - s_record_start_time;
                ESP_LOGI(TAG, "VAD STOP: rms=%d, duration=%lldms, chunks=%lu",
                         rms, (long long)record_duration, (unsigned long)s_total_chunks);

                s_state = VAD_STATE_IDLE;
                s_voice_count = 0;
                s_silence_count = 0;
                s_last_stop_time = now;

                if (s_event_cb) {
                    s_event_cb(VAD_EVENT_STOP, rms, s_user_ctx);
                }
            }
            break;
        }
    }
}

vad_state_t vad_get_state(void)
{
    return s_state;
}

int vad_get_volume(void)
{
    return s_last_volume;
}

bool vad_is_recording(void)
{
    return (s_state == VAD_STATE_LISTENING || s_state == VAD_STATE_ENDING);
}

void vad_force_stop(void)
{
    if (s_state != VAD_STATE_IDLE) {
        ESP_LOGW(TAG, "VAD 强制停止，当前状态=%d", s_state);
        s_state = VAD_STATE_IDLE;
        s_voice_count = 0;
        s_silence_count = 0;
        s_last_stop_time = get_time_ms();
    }
}

void vad_set_enabled(bool enabled)
{
    if (s_enabled != enabled) {
        s_enabled = enabled;
        ESP_LOGI(TAG, "VAD %s", enabled ? "启用" : "禁用");
        if (!enabled) {
            // 禁用时重置状态
            s_voice_count = 0;
            s_silence_count = 0;
        }
    }
}

void vad_get_stats(uint32_t *total_recordings, uint32_t *total_chunks)
{
    if (total_recordings) *total_recordings = s_total_recordings;
    if (total_chunks)     *total_chunks     = s_total_chunks;
}
