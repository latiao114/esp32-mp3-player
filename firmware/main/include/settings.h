/*
 * settings —— 用户设置（NVS 持久化）
 *
 * 只存 7 个 key，和 UI与交互设计.md 第 4 节一致。
 * 写 NVS 会阻塞几十 ms，所以只在用户改设置时调用 settings_save()。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t dark;       /* 0=浅色 1=深色 */
    uint8_t online;     /* 0=关 1=开（热点 + 网页 + 联网） */
    uint8_t bootanim;   /* 0=关 1=开 */
    uint8_t vol;        /* 0~100 */
    uint8_t bright;     /* 0~100 */
    uint8_t mode;       /* 0=顺序 1=单曲 2=随机 */
    uint8_t screenoff;  /* 0=15s 1=30s 2=1min 3=5min */
    uint8_t rot;        /* 屏幕方向 0~3；255 = 还没定，下次开机进自检 */
} settings_t;

extern settings_t g_set;

/** @brief 从 NVS 读设置，没有就用默认值 */
void settings_load(void);

/** @brief 写回 NVS（失败不 abort，只打日志） */
void settings_save(void);

/** @brief 息屏时间（秒） */
int settings_screenoff_sec(void);

#ifdef __cplusplus
}
#endif
