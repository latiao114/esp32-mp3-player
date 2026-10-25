/*
 * buttons —— 屏幕模块自带 4 键
 */

#include "buttons.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_timer.h"

/* 注意：本屏模块的 K1/K2 与丝印相反 ——
 * 接在 GPIO8 上的按键实际是“上”，GPIO7 上的实际是“下”。
 * 这里按实际按键方向配置；更换模块时请重新核对。 */
#define PIN_K1  8     /* 上 */
#define PIN_K2  7     /* 下 */
#define PIN_K3  9     /* #  井 */
#define PIN_K4  14    /* *  星 */

#define DEBOUNCE_MS  20

typedef struct {
    int     gpio;
    btn_id_t id;
    bool    raw;          /* 最近一次读到的电平（true=按下） */
    bool    stable;       /* 消抖确认后的状态 */
    int64_t changed_at;   /* 电平最后一次变化的时间(ms) */
} btn_state_t;

static btn_state_t s_btns[] = {
    { PIN_K1, BTN_DOWN, false, false, 0 },
    { PIN_K2, BTN_UP,   false, false, 0 },
    { PIN_K3, BTN_HASH, false, false, 0 },
    { PIN_K4, BTN_STAR, false, false, 0 },
};

#define BTN_COUNT  (sizeof(s_btns) / sizeof(s_btns[0]))

void buttons_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_K1) | (1ULL << PIN_K2) |
                        (1ULL << PIN_K3) | (1ULL << PIN_K4),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* 按键另一端接 GND */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void buttons_update(void)
{
    int64_t now = esp_timer_get_time() / 1000;

    for (size_t i = 0; i < BTN_COUNT; i++) {
        /* 低电平 = 按下 */
        bool raw = (gpio_get_level(s_btns[i].gpio) == 0);

        if (raw != s_btns[i].raw) {
            s_btns[i].raw        = raw;
            s_btns[i].changed_at = now;
        }
        if (raw != s_btns[i].stable && (now - s_btns[i].changed_at) >= DEBOUNCE_MS) {
            s_btns[i].stable = raw;
        }
    }
}

btn_id_t buttons_read(void)
{
    buttons_update();
    for (size_t i = 0; i < BTN_COUNT; i++) {
        if (s_btns[i].stable) {
            return s_btns[i].id;
        }
    }
    return BTN_NONE;
}

btn_id_t buttons_pressed(void)
{
    static btn_id_t prev = BTN_NONE;

    buttons_update();

    btn_id_t cur = BTN_NONE;
    for (size_t i = 0; i < BTN_COUNT; i++) {
        if (s_btns[i].stable) {
            cur = s_btns[i].id;
            break;
        }
    }

    btn_id_t ev = (cur != BTN_NONE && cur != prev) ? cur : BTN_NONE;
    prev = cur;
    return ev;
}

const char *buttons_name(btn_id_t id)
{
    switch (id) {
    case BTN_DOWN: return "DOWN";
    case BTN_UP:   return "UP";
    case BTN_HASH: return "HASH(#)";
    case BTN_STAR: return "STAR(*)";
    default:       return "-";
    }
}
