/*
 * audio_player —— I2S 输出 + minimp3 软解（流式）
 *
 * 硬件：
 *   PCM5102A：BCK=GPIO4，LRCK=GPIO5，DIN=GPIO6，SCK 接 GND（用内部 PLL）
 *   ⚠️ XSMT 必须接 3V3，否则芯片静音
 *
 * 解码器：minimp3（单文件，MIT）
 * 这里是「边读边解」的流式实现：一个独立任务按块读文件 → 解码 → 写 I2S。
 * 这样长歌不占内存，暂停/切歌也能立刻响应。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_IDLE = 0,   /* 没在播 */
    AUDIO_PLAYING,
    AUDIO_PAUSED,
} audio_state_t;

/** @brief 初始化 I2S + 启动播放任务（默认 44100Hz / 16bit / 立体声） */
esp_err_t audio_init(void);

/**
 * @brief 开始播放一个文件（异步，立即返回）
 * @param path        完整路径
 * @param duration_ms 时长（用来算进度条，可传 0）
 */
esp_err_t audio_play(const char *path, uint32_t duration_ms);

void audio_pause(void);
void audio_resume(void);
void audio_stop(void);

/** @brief 软件音量 0~100（PCM5102A 没有硬件音量） */
void audio_set_volume(uint8_t vol);

audio_state_t audio_state(void);
uint32_t      audio_pos_ms(void);
uint32_t      audio_duration_ms(void);

#ifdef __cplusplus
}
#endif
