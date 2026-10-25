/*
 * keys —— 按键事件状态机
 */

#include "keys.h"

#include <string.h>

#include "buttons.h"
#include "esp_timer.h"

#define LONG_MS    600
#define REPEAT_MS  120
#define DOUBLE_MS  300

typedef struct {
    btn_id_t id;
    bool     has_double;
    bool     down;
    bool     long_sent;
    bool     suppress_click;   /* 双击的第二下，松手不再产生单击 */
    bool     wait_double;
    int64_t  down_ms;
    int64_t  release_ms;
    int64_t  next_repeat;
} key_t;

static key_t s_keys[] = {
    { BTN_UP,   false, false, false, false, false, 0, 0, 0 },
    { BTN_DOWN, false, false, false, false, false, 0, 0, 0 },
    { BTN_STAR, true,  false, false, false, false, 0, 0, 0 },
    { BTN_HASH, true,  false, false, false, false, 0, 0, 0 },
};
#define KEY_COUNT  (sizeof(s_keys) / sizeof(s_keys[0]))

/* 每个键的双击识别开关；菜单里把 * 关掉，让单击立刻生效 */
static bool s_double_on[4] = { true, true, true, true };

void keys_set_double(btn_id_t id, bool enable)
{
    int idx = (id == BTN_UP) ? 0 : (id == BTN_DOWN) ? 1 : (id == BTN_STAR) ? 2 : 3;
    s_double_on[idx] = enable;

    if (!enable) {
        /* 关掉时取消等待中的双击窗口，避免过 300ms 又补发一次单击 */
        for (size_t i = 0; i < KEY_COUNT; i++) {
            if (s_keys[i].id == id) {
                s_keys[i].wait_double = false;
            }
        }
    }
}

#define QSIZE  16
static key_event_t s_q[QSIZE];
static int         s_head, s_tail;

static void push(key_event_t ev)
{
    int next = (s_head + 1) % QSIZE;
    if (next == s_tail) {
        return;                 /* 满了就丢，按键事件不排长队 */
    }
    s_q[s_head] = ev;
    s_head      = next;
}

static key_event_t click_of(btn_id_t id)
{
    switch (id) {
    case BTN_UP:   return KEY_UP;
    case BTN_DOWN: return KEY_DOWN;
    case BTN_STAR: return KEY_STAR;
    case BTN_HASH: return KEY_HASH;
    default:       return KEY_NONE;
    }
}

static key_event_t repeat_of(btn_id_t id)
{
    switch (id) {
    case BTN_UP:   return KEY_UP_REPEAT;
    case BTN_DOWN: return KEY_DOWN_REPEAT;
    case BTN_STAR: return KEY_STAR_LONG;
    case BTN_HASH: return KEY_HASH_LONG;
    default:       return KEY_NONE;
    }
}

static key_event_t double_of(btn_id_t id)
{
    return (id == BTN_STAR) ? KEY_STAR_DOUBLE : KEY_HASH_DOUBLE;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void keys_init(void)
{
    buttons_init();
    memset(s_keys, 0, sizeof(s_keys));
    for (size_t i = 0; i < KEY_COUNT; i++) {
        s_keys[i].id = (btn_id_t)(i == 0 ? BTN_UP :
                                  i == 1 ? BTN_DOWN :
                                  i == 2 ? BTN_STAR : BTN_HASH);
        s_keys[i].has_double = (s_keys[i].id == BTN_STAR || s_keys[i].id == BTN_HASH);
    }
    s_head = s_tail = 0;
}

void keys_poll(void)
{
    int64_t  now = now_ms();
    btn_id_t cur = buttons_read();

    for (size_t i = 0; i < KEY_COUNT; i++) {
        key_t *k       = &s_keys[i];
        bool   pressed = (cur == k->id);
        bool   dbl     = k->has_double && s_double_on[i];

        if (pressed && !k->down) {
            k->down    = true;
            k->down_ms = now;
            k->long_sent = false;

            if (k->wait_double) {
                /* 上一次松手后 300ms 内又按下 → 双击 */
                k->wait_double   = false;
                k->suppress_click = true;
                push(double_of(k->id));
            }
        } else if (!pressed && k->down) {
            k->down = false;

            if (!k->long_sent && !k->suppress_click) {
                if (dbl) {
                    k->wait_double = true;
                    k->release_ms  = now;
                } else {
                    push(click_of(k->id));   /* 双击关掉的键立刻发 */
                }
            }
            k->suppress_click = false;
        }

        /* 长按：到 600ms 发一次。
           UP/DOWN 继续每 120ms 连发（音量、列表滚动需要）；
           * 和 # 只发一次 —— 否则按住不放会一直切播放模式。 */
        if (k->down && !k->long_sent && (now - k->down_ms) >= LONG_MS) {
            k->long_sent   = true;
            k->next_repeat = now + REPEAT_MS;
            push(repeat_of(k->id));
        } else if (k->down && k->long_sent &&
                   (k->id == BTN_UP || k->id == BTN_DOWN) &&
                   now >= k->next_repeat) {
            k->next_repeat = now + REPEAT_MS;
            push(repeat_of(k->id));
        }

        /* 双击窗口到期 → 补发单击 */
        if (k->wait_double && (now - k->release_ms) >= DOUBLE_MS) {
            k->wait_double = false;
            push(click_of(k->id));
        }
    }
}

bool keys_next(key_event_t *ev)
{
    if (s_tail == s_head) {
        return false;
    }
    *ev    = s_q[s_tail];
    s_tail = (s_tail + 1) % QSIZE;
    return true;
}

void keys_flush(void)
{
    s_tail = s_head;
    for (size_t i = 0; i < KEY_COUNT; i++) {
        s_keys[i].wait_double   = false;
        /* ⚠️ 正按着的键，松手时也不能补发单击，长按也不能连发。
           息屏时按一下唤醒，如果不屏蔽，松手就变成"切下一首"，
           按住不放还会一直调音量。 */
        if (s_keys[i].down) {
            s_keys[i].suppress_click = true;
            s_keys[i].long_sent      = true;
        } else {
            s_keys[i].suppress_click = false;
        }
    }
}
