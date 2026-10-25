/*
 * buttons —— 屏幕模块自带的 4 个按键
 *
 * 模块排针（从左到右）：
 *   GND VCC SCL SDA RES DC BLK K4 K3 K2 K1
 *
 * 按键丝印与含义：
 *   K1 = v  向下
 *   K2 = ^  向上
 *   K3 = #  井号
 *   K4 = *  星号
 *
 * 接法：内部上拉，按下导通到 GND（低电平有效）。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_NONE = 0,
    BTN_DOWN,   /* K1 */
    BTN_UP,     /* K2 */
    BTN_HASH,   /* K3 */
    BTN_STAR,   /* K4 */
} btn_id_t;

/** @brief 初始化 4 个按键引脚（内部上拉） */
void buttons_init(void);

/**
 * @brief 读当前按下的键（带 20ms 消抖）
 * @return 按下的键；同时按多个时返回优先级最高的一个；没按返回 BTN_NONE
 */
btn_id_t buttons_read(void);

/** @brief 取一次「刚按下」的边沿事件，没有则返回 BTN_NONE（适合放主循环里） */
btn_id_t buttons_pressed(void);

/** @brief 按键名，调试打印用 */
const char *buttons_name(btn_id_t id);

#ifdef __cplusplus
}
#endif
