/*
 * keys —— 4 个按键的事件化封装
 *
 * 硬件层（buttons.c）只给「现在哪个键按着」，这里补上单击 / 长按 / 连发 / 双击：
 *   消抖 20ms（buttons 内部）、长按 600ms、连发 120ms、双击窗口 300ms
 *
 * ⚠️ 带双击的键（* 和 #）单击要延迟 300ms 才发，否则双击会被拆成两次单击。
 */
#pragma once

#include <stdbool.h>

#include "buttons.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KEY_NONE = 0,
    KEY_UP,            /* ^  单击 */
    KEY_UP_REPEAT,     /* ^  长按（含连发） */
    KEY_DOWN,          /* v  单击 */
    KEY_DOWN_REPEAT,   /* v  长按 */
    KEY_STAR,          /* *  单击（延迟 300ms） */
    KEY_STAR_LONG,     /* *  长按 */
    KEY_STAR_DOUBLE,   /* *  双击 */
    KEY_HASH,          /* #  单击（延迟 300ms） */
    KEY_HASH_LONG,     /* #  长按 */
    KEY_HASH_DOUBLE,   /* #  双击 */
} key_event_t;

/** @brief 初始化（内部调用 buttons_init） */
void keys_init(void);

/** @brief 每 20ms 调一次，扫描并生成事件 */
void keys_poll(void);

/** @brief 取一个待处理事件，没有返回 false */
bool keys_next(key_event_t *ev);

/** @brief 丢掉所有积压事件（息屏唤醒后防误触） */
void keys_flush(void);

/**
 * @brief 临时打开/关闭某个键的双击识别
 *
 * 关掉之后单击**立即**触发，不再等 300ms 双击窗口。
 * 菜单里用 * 调整选项时关掉它的双击，否则连按两下会被并成一次双击。
 */
void keys_set_double(btn_id_t id, bool enable);

#ifdef __cplusplus
}
#endif
