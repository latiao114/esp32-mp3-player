/*
 * ui —— 界面实现
 */

#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>

#include "audio_player.h"
#include "buttons.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "font_data.h"
#include "keys.h"
#include "sd_card.h"
#include "settings.h"
#include "tft_display.h"
#include "wifi_prov.h"

static const char *TAG = "ui";

#define C565(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define SCREEN_W  240
#define SCREEN_H  240

typedef struct {
    uint16_t bg;
    uint16_t panel;
    uint16_t text;
    uint16_t text2;
    uint16_t accent;
    uint16_t line;
    uint16_t hi;        /* 选中行底色：比 bg 明显偏绿 */
} theme_t;

static theme_t s_theme;

typedef enum {
    SCR_ORIENT = 0,   /* 首次开机：选屏幕方向 */
    SCR_BOOT,
    SCR_PLAYER,
    SCR_MENU,
    SCR_LIST,
} screen_t;

static screen_t s_screen = SCR_BOOT;
static int      s_sel;            /* 菜单/选歌的选中项 */
static int      s_list_top;       /* 选歌列表滚动位置 */
static int      s_orient;         /* 方向自检里的临时选择 */

static sd_file_t s_files[100];
static int       s_count;
static bool      s_sd_ready;

static int  s_cur = -1;           /* 当前曲目 */
static bool s_playing;            /* 是否处于播放会话中 */
static int64_t s_play_at;

static char s_title[96];
static int  s_title_w;
static int  s_marquee;
static int64_t s_last_marq;
static int  s_title_alpha = 100;   /* 切歌时歌名从 0 淡入 */

static int     s_disc_angle;
static int64_t s_last_disc;
static int64_t s_last_prog;
static int64_t s_last_time;
static int64_t s_last_list_anim;
static int     s_menu_hi     = 28;
static int     s_menu_target = 28;
static int64_t s_last_menu_anim;
static int     s_ui_off;           /* 界面滑入偏移 */
static int64_t s_last_ip_check;
static int     s_last_wifi_st = -1; /* 菜单里盯着 WiFi 状态，连上后自动刷 IP 行 */
static int64_t s_last_sd_check;     /* SD 卡热插拔检测 */

/* 三个开关的拨片位置（0=左 1=右），逐帧插值 */
static float s_toggle_pos[3]    = { 0.0f, 0.0f, 1.0f };
static bool  s_toggle_target[3] = { false, false, true };
static int64_t s_vol_dirty;      /* 音量改过、还没写 NVS */

static int64_t s_last_activity;
static bool    s_asleep;
static int64_t s_boot_at;

static uint16_t s_discbuf[96 * 96];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int isqrt(int v)
{
    if (v <= 0) {
        return 0;
    }
    int r = 0;
    while ((r + 1) * (r + 1) <= v) {
        r++;
    }
    return r;
}

static void fill_round(int x, int y, int w, int h, int r, uint16_t c, uint16_t bg)
{
    tft_fill_round(x, y, w, h, r, c, bg);
}

static void text_right(int right, int y, const char *s, uint16_t fg, uint16_t bg)
{
    tft_text(right - font_text_width(s), y, s, fg, bg);
}

/* ================================================================= */
/* 主题                                                               */
/* ================================================================= */
static void theme_update(void)
{
    if (g_set.dark) {
        s_theme.bg     = C565(26, 26, 26);
        s_theme.panel  = C565(43, 43, 43);
        s_theme.text   = C565(236, 236, 236);
        s_theme.text2  = C565(154, 154, 154);
        s_theme.accent = C565(93, 161, 63);
        s_theme.line   = C565(58, 58, 58);
        s_theme.hi     = C565(40, 62, 30);      /* 深绿底，选中一眼能看出来 */
    } else {
        s_theme.bg     = C565(242, 242, 242);
        s_theme.panel  = C565(255, 255, 255);
        s_theme.text   = C565(26, 26, 26);
        s_theme.text2  = C565(102, 102, 102);
        s_theme.accent = C565(74, 131, 49);
        s_theme.line   = C565(210, 210, 210);
        s_theme.hi     = C565(214, 234, 196);   /* 浅绿底 */
    }
}

/* ================================================================= */
/* 小控件                                                             */
/* ================================================================= */
static void draw_battery(int x, int y)
{
    tft_rect(x, y, x + 20, y + 12, s_theme.text2);
    tft_fill_rect(x + 20, y + 3, x + 22, y + 9, s_theme.text2);
    tft_fill_rect(x + 2, y + 2, x + 18, y + 10, s_theme.accent);
}

static void draw_wifi(int x, int y)
{
    wifi_state_t st    = wifi_prov_state();
    int          level = 0;
    if (st == WIFI_STATE_CONNECTED) {
        level = 3;
    } else if (st == WIFI_STATE_CONNECTING) {
        level = 2;
    } else if (st == WIFI_STATE_FAILED) {
        level = 1;
    }

    for (int i = 0; i < 3; i++) {
        int      h = 5 + i * 5;
        uint16_t c = (i < level) ? s_theme.accent : s_theme.line;
        tft_fill_rect(x + i * 7, y + 15 - h, x + i * 7 + 4, y + 15, c);
    }
}

static void draw_toggle(int idx, int x, int y, bool on, uint16_t bg)
{
    /* 直角矩形：12px 高的圆角在这块 240×240 屏上全是锯齿 */
    tft_fill_rect(x - 2, y - 2, x + 34, y + 18, bg);
    tft_fill_rect(x, y, x + 32, y + 16, on ? s_theme.accent : s_theme.line);

    int cx = x + 2 + (int)(s_toggle_pos[idx] * 16.0f);
    tft_fill_rect(cx, y + 2, cx + 12, y + 14,
                  on ? C565(255, 255, 255) : s_theme.text2);
}

static void draw_statusbar(void)
{
    tft_fill_rect(0, 0, SCREEN_W, 26, s_theme.panel);
    tft_fill_rect(0, 26, SCREEN_W, 27, s_theme.line);

    const char *m = g_set.mode == 0 ? "顺序" : (g_set.mode == 1 ? "单曲" : "随机");
    tft_text(6, 1, m, s_theme.accent, s_theme.panel);

    draw_wifi(176, 4);
    draw_battery(208, 9);
}

/* ================================================================= */
/* 唱片                                                               */
/* ================================================================= */
static void render_disc(void)
{
    const int S = 96, cx = 48, cy = 48, r = 46;
    bool      spin = (audio_state() == AUDIO_PLAYING);

    float  a  = (float)s_disc_angle * 3.14159265f / 180.0f;
    int    mx = cx + (int)(cosf(a) * 26.0f);
    int    my = cy + (int)(sinf(a) * 26.0f);

    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            int      dx = x - cx, dy = y - cy;
            int      d2 = dx * dx + dy * dy;
            uint16_t c  = s_theme.bg;

            if (d2 <= r * r) {
                int d = isqrt(d2);
                if (d >= r - 1) {
                    c = C565(78, 78, 82);                 /* 外沿 */
                } else if (d < 3) {
                    c = s_theme.bg;                        /* 中心孔 */
                } else if (d < 15) {
                    c = s_theme.accent;                    /* 标签 */
                } else if (d < 17) {
                    c = C565(200, 200, 200);
                } else {
                    c = ((d / 3) & 1) ? C565(32, 32, 35) : C565(22, 22, 25);

                    float ang = atan2f((float)dy, (float)dx) * 57.29578f;
                    int   rel = ((int)ang + 360) % 360;

                    /* 随唱片转的径向细纹：同心圆转起来看不出来，
                       必须有点"随角度走"的东西才能看出盘子在转 */
                    int spin_rel = (rel - s_disc_angle + 360) % 360;
                    if ((spin_rel % 40) < 4 && d > 18) {
                        c = C565(46, 46, 50);
                    }

                    /* 胶片反光：固定光源。角度上做高斯状衰减，
                       径向上越靠外越亮，形成一道斜向柔光。 */
                    if (rel >= 36 && rel <= 80) {
                        int e   = 58 - abs(rel - 58);      /* 0..22 */
                        int rad = (d - 17) * 3 / 10;       /* 0..8 */
                        int v   = 38 + e * 2 + rad;        /* 38..90 */
                        if (v > 255) {
                            v = 255;
                        }
                        c = C565(v, v, v);
                    }
                }
                if (abs(x - mx) <= 2 && abs(y - my) <= 2 && d >= 17) {
                    c = C565(235, 235, 235);               /* 转动的小白点 */
                }
            }
            s_discbuf[y * S + x] = c;
        }
    }
}

static void draw_disc(void)
{
    render_disc();
    tft_blit(72, 32, 96, 96, s_discbuf);
}

/* ================================================================= */
/* 音量条                                                             */
/* ================================================================= */
static float s_vol_disp = 60.0f;   /* 音量条的显示值，向 g_set.vol 平滑逼近 */

static void draw_volume(void)
{
    /* 高度只到 124：再往下就跟歌名区（130 起）重叠，
       歌名滚动重绘时会把音量条底部切掉，看起来像往下滑 */
    const int x = 220, y0 = 40, w = 8, h = 84;

    tft_fill_rect(x, y0, x + w, y0 + h, s_theme.line);

    int fh = h * (int)(s_vol_disp + 0.5f) / 100;
    if (fh > 0) {
        tft_fill_rect(x, y0 + h - fh, x + w, y0 + h, s_theme.accent);
    }
}

/* ================================================================= */
/* 播放界面                                                           */
/* ================================================================= */
static void set_title(const char *filename)
{
    char tmp[sizeof(s_title)];
    strncpy(tmp, filename, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *dot = strrchr(tmp, '.');
    if (dot) {
        *dot = '\0';
    }
    strncpy(s_title, tmp, sizeof(s_title) - 1);
    s_title[sizeof(s_title) - 1] = '\0';
    s_title_w = font_text_width(s_title);
    s_marquee = 0;
}

/* 把颜色 a 按 pct(0~100) 混合向 b，用于淡入 */
static uint16_t mix_color(uint16_t a, uint16_t b, int pct)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;

    int r = ar + (br - ar) * pct / 100;
    int g = ag + (bg - ag) * pct / 100;
    int bl = ab + (bb - ab) * pct / 100;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void draw_title(void)
{
    tft_fill_rect(0, 130, SCREEN_W, 158, s_theme.bg);

    uint16_t fg = (s_title_alpha >= 100)
                  ? s_theme.text
                  : mix_color(s_theme.bg, s_theme.text, s_title_alpha);

    if (s_title_w <= 216) {
        tft_text((SCREEN_W - s_title_w) / 2, 132, s_title, fg, s_theme.bg);
    } else {
        tft_text(12 - s_marquee, 132, s_title, fg, s_theme.bg);
        tft_text(12 - s_marquee + s_title_w + 40, 132, s_title, fg, s_theme.bg);
    }
}

static void draw_artist(void)
{
    const char *a = "本地音乐";
    tft_text((SCREEN_W - font_text_width(a)) / 2, 158, a, s_theme.text2, s_theme.bg);
}

static void fmt_time(char *buf, size_t sz, uint32_t ms)
{
    unsigned sec = ms / 1000;
    snprintf(buf, sz, "%u:%02u", sec / 60, sec % 60);
}

static void draw_times(void)
{
    char a[16], b[16];
    fmt_time(a, sizeof(a), audio_pos_ms());
    fmt_time(b, sizeof(b), audio_duration_ms());

    tft_text(12, 196, a, s_theme.text2, s_theme.bg);
    text_right(228, 196, b, s_theme.text2, s_theme.bg);
}

static void draw_progress(void)
{
    uint32_t pos = audio_pos_ms();
    uint32_t dur = audio_duration_ms();

    tft_fill_rect(12, 186, 228, 192, s_theme.line);
    if (dur > 0) {
        int w = (int)((uint64_t)pos * 216 / dur);
        if (w > 216) {
            w = 216;
        }
        if (w > 0) {
            tft_fill_rect(12, 186, 12 + w, 192, s_theme.accent);
        }
    }
}

/* 没有 SD 卡时的整页：不画唱片，中间一张卡的图标 + 两行说明 */
static void draw_no_sd(void)
{
    const int cx = SCREEN_W / 2;
    const int iy = 44;

    /* SD 卡简笔图标：圆角卡体 + 左上缺角 + 底部金手指 */
    fill_round(cx - 26, iy, 52, 66, 6, s_theme.panel, s_theme.bg);
    tft_fill_rect(cx - 26, iy, cx - 10, iy + 18, s_theme.bg);

    for (int i = 0; i < 4; i++) {
        tft_fill_rect(cx - 16 + i * 9, iy + 48, cx - 10 + i * 9, iy + 60, s_theme.text2);
    }

    const char *t1 = "未检测到 SD 卡";
    const char *t2 = "插入后重启设备";
    tft_text((SCREEN_W - font_text_width(t1)) / 2, 136, t1, s_theme.text, s_theme.bg);
    tft_text((SCREEN_W - font_text_width(t2)) / 2, 162, t2, s_theme.text2, s_theme.bg);
}

static void draw_player(void)
{
    tft_fill(s_theme.bg);
    draw_statusbar();

    if (s_count == 0) {
        draw_no_sd();
        return;
    }

    draw_disc();
    draw_volume();
    draw_title();
    draw_artist();
    draw_progress();
    draw_times();
}

/* ================================================================= */
/* 菜单                                                               */
/* ================================================================= */
static const char *const k_menu[] = {
    "在线模式", "深色模式", "开机动画", "屏幕亮度",
    "息屏时间", "IP", "SD 卡", "固件",
};
#define MENU_N  (int)(sizeof(k_menu) / sizeof(k_menu[0]))

static void menu_value(int i, int y, uint16_t bg)
{
    char buf[48];

    switch (i) {
    case 0:
        draw_toggle(0, 196, y + 5, g_set.online != 0, bg);
        break;
    case 1:
        draw_toggle(1, 196, y + 5, g_set.dark != 0, bg);
        break;
    case 2:
        draw_toggle(2, 196, y + 5, g_set.bootanim != 0, bg);
        break;
    case 3:
        snprintf(buf, sizeof(buf), "%d", g_set.bright);
        text_right(228, y + 1, buf, s_theme.text2, bg);
        break;
    case 4: {
        static const char *const t[] = { "15秒", "30秒", "1分钟", "5分钟" };
        int k = g_set.screenoff > 3 ? 1 : g_set.screenoff;
        text_right(228, y + 1, t[k], s_theme.text2, bg);
        break;
    }
    case 5: {
        /* 只有「在线模式开着 + 真的连上了」才显示 IP，
           否则关掉在线模式还挂着地址，看着像设置没生效 */
        const char *ip = "--";
        if (g_set.online && wifi_prov_state() == WIFI_STATE_CONNECTED &&
            wifi_prov_ip()[0]) {
            ip = wifi_prov_ip();
        }
        text_right(228, y + 1, ip, s_theme.text2, bg);
        break;
    }
    case 6:
        text_right(228, y + 1, s_sd_ready ? "已就绪" : "未检测到", s_theme.text2, bg);
        break;
    case 7:
        text_right(228, y + 1, esp_app_get_description()->version, s_theme.text2, bg);
        break;
    default:
        break;
    }
}

static void draw_menu_row(int i)
{
    int      y  = 28 + i * 26;
    uint16_t bg = (i == s_sel) ? s_theme.panel : s_theme.bg;

    if (i == s_sel) {
        fill_round(4, y, 232, 26, 6, s_theme.panel, s_theme.bg);
    } else {
        tft_fill_rect(4, y, 236, y + 26, s_theme.bg);
    }
    tft_text(12, y + 1, k_menu[i], (i == s_sel) ? s_theme.accent : s_theme.text, bg);
    menu_value(i, y, bg);
}

static void draw_menu(void)
{
    tft_fill(s_theme.bg);

    const char *t = "设置";
    tft_text((SCREEN_W - font_text_width(t)) / 2, 2, t, s_theme.text, s_theme.bg);

    /* 先把非高亮行的底色清掉 */
    for (int i = 0; i < MENU_N; i++) {
        if (i == s_sel) {
            continue;
        }
        int y = 28 + i * 26;
        tft_fill_rect(4, y, 236, y + 26, s_theme.bg);
    }

    /* 高亮框画在动画位置上（s_menu_hi 会逐帧逼近目标行） */
    fill_round(4, s_menu_hi, 232, 26, 6, s_theme.hi, s_theme.bg);
    /* 左侧强调竖条：只有底色的话远看还是糊成一片 */
    tft_fill_rect(4, s_menu_hi + 4, 9, s_menu_hi + 22, s_theme.accent);

    for (int i = 0; i < MENU_N; i++) {
        int      y  = 28 + i * 26;
        uint16_t bg = (i == s_sel) ? s_theme.hi : s_theme.bg;

        tft_text(12, y + 1, k_menu[i], (i == s_sel) ? s_theme.accent : s_theme.text, bg);
        menu_value(i, y, bg);
    }
}

/* 只改选中项，高亮框由 menu_anim_tick 逐帧滑过去 */
static void menu_move(int d)
{
    s_sel         = (s_sel + d + MENU_N) % MENU_N;
    s_menu_target = 28 + s_sel * 26;
}

/* 只重画 IP 那一行的值：连上 WiFi 之后不用退出菜单再进来 */
static void refresh_ip_row(void)
{
    int      y  = 28 + 5 * 26;
    uint16_t bg = (s_sel == 5) ? s_theme.hi : s_theme.bg;
    tft_fill_rect(60, y, 232, y + 26, bg);
    menu_value(5, y, bg);
}

static void menu_anim_tick(void){
    bool changed = false;

    if (s_menu_hi != s_menu_target) {
        int diff = s_menu_target - s_menu_hi;
        s_menu_hi += (diff > 0) ? (diff / 2 + 1) : (diff / 2 - 1);
        if (abs(s_menu_target - s_menu_hi) <= 1) {
            s_menu_hi = s_menu_target;
        }
        changed = true;
    }

    /* 开关拨片滑过去 */
    for (int i = 0; i < 3; i++) {
        float target = s_toggle_target[i] ? 1.0f : 0.0f;
        float diff   = target - s_toggle_pos[i];
        if (fabsf(diff) > 0.02f) {
            s_toggle_pos[i] += diff * 0.35f;
            changed = true;
        } else if (s_toggle_pos[i] != target) {
            s_toggle_pos[i] = target;
            changed = true;
        }
    }

    if (changed) {
        draw_menu();
    }
}

/* ================================================================= */
/* 选歌                                                               */
/* ================================================================= */
static void truncate_to_width(const char *src, char *dst, size_t dstsz, int max_w)
{
    size_t out = 0;
    int    w   = 0;

    for (const char *p = src; *p;) {
        int      n;
        uint32_t cp = font_utf8_next(p, &n);
        int      cw = (cp < 0x80) ? FONT_ASCII_W : FONT_W;
        if (w + cw > max_w || out + (size_t)n + 1 >= dstsz) {
            break;
        }
        memcpy(dst + out, p, (size_t)n);
        out += (size_t)n;
        w   += cw;
        p   += n;
    }
    dst[out] = '\0';
}

static int list_visible(void)
{
    return 6;
}

static void list_fix_top(void)
{
    int max_top = s_count - list_visible();
    if (max_top < 0) {
        max_top = 0;
    }
    if (s_sel < s_list_top) {
        s_list_top = s_sel;
    }
    if (s_sel >= s_list_top + list_visible()) {
        s_list_top = s_sel - list_visible() + 1;
    }
    if (s_list_top > max_top) {
        s_list_top = max_top;
    }
    if (s_list_top < 0) {
        s_list_top = 0;
    }
}

/* 选歌：高亮框固定在第 3 行，列表在它下面滚，首尾循环（iPod 那种手感） */
#define LIST_ROWS   5
#define LIST_MID    2
#define LIST_ROW_H  28
#define LIST_TOP_Y  100

static int s_list_off;          /* 滑动动画的像素偏移，0 = 对齐 */

static void draw_list_row(int i)
{
    if (s_count <= 0) {
        return;
    }

    int idx = (s_sel - LIST_MID + i + s_count * 1000) % s_count;
    int y   = LIST_TOP_Y + (i - LIST_MID) * LIST_ROW_H + s_list_off;

    if (y < 28 || y + FONT_H > 204) {
        return;                 /* 裁剪到列表区域内，避免遮挡「选歌」标题 */
    }

    char name[64];
    truncate_to_width(s_files[idx].name, name, sizeof(name), 200);

    uint16_t bg = (i == LIST_MID) ? s_theme.hi : s_theme.bg;
    tft_text(12, y + 2, name,
             (i == LIST_MID) ? s_theme.accent : s_theme.text, bg);
}

static void draw_list_ex(bool full)
{
    if (full) {
        tft_fill(s_theme.bg);
    } else {
        /* 只清列表区（到 200，不含底部「共 N 首」），
           否则动画每帧都把提示擦掉重画，看着在抽 */
        tft_fill_rect(0, 20, SCREEN_W, 200, s_theme.bg);
    }

    if (s_count == 0) {
        const char *msg = "未检测到歌曲";
        tft_text((SCREEN_W - font_text_width(msg)) / 2, 100, msg, s_theme.text2, s_theme.bg);
        return;
    }

    /* 高亮框跟着内容一起滑（否则选中行文字带着底色滑走，框留在原地，
       看起来就是"绿色分家了"）；动画结束 s_list_off=0，正好回中 */
    int hi_y = LIST_TOP_Y + s_list_off;
    fill_round(4, hi_y, 232, LIST_ROW_H, 6, s_theme.hi, s_theme.bg);
    tft_fill_rect(4, hi_y + 4, 9, hi_y + LIST_ROW_H - 4, s_theme.accent);

    /* 上下各多画一行：滚动时新歌要提前就在画面里，
       否则 5 行整块上飘一下再弹回来，看着就是抽 */
    for (int i = -1; i <= LIST_ROWS; i++) {
        draw_list_row(i);
    }

    /* 标题和底部提示最后画：行滑出去时会被盖住，不会突然消失 */
    const char *t = "选歌";
    tft_text((SCREEN_W - font_text_width(t)) / 2, 2, t, s_theme.text, s_theme.bg);

    if (full) {
        char buf[32];
        snprintf(buf, sizeof(buf), "共 %d 首", s_count);
        text_right(228, 204, buf, s_theme.text2, s_theme.bg);
    }
}

static void draw_list(void)
{
    draw_list_ex(true);
}

/* ================================================================= */
/* 开机动画                                                           */
/* ================================================================= */
static void draw_orient(void)
{
    tft_fill(s_theme.bg);

    const char *t = "音乐播放器";
    tft_text((SCREEN_W - font_text_width(t)) / 2, 62, t, s_theme.text, s_theme.bg);

    char buf[24];
    snprintf(buf, sizeof(buf), "方向 %d", s_orient + 1);
    tft_text((SCREEN_W - font_text_width(buf)) / 2, 110, buf, s_theme.accent, s_theme.bg);

    const char *h = "按 ^ v 切换";
    tft_text((SCREEN_W - font_text_width(h)) / 2, 146, h, s_theme.text2, s_theme.bg);

    const char *h2 = "按 * 确认";
    tft_text((SCREEN_W - font_text_width(h2)) / 2, 176, h2, s_theme.text2, s_theme.bg);
}

static void draw_boot(void)
{
    tft_fill(s_theme.bg);

    s_disc_angle = 0;
    render_disc();
    tft_blit(72, 50, 96, 96, s_discbuf);

    const char *t = "音乐播放器";
    tft_text((SCREEN_W - font_text_width(t)) / 2, 166, t, s_theme.accent, s_theme.bg);
}

/* ================================================================= */
/* 重绘                                                               */
/* ================================================================= */
static void redraw(void)
{
    tft_set_offset_y(s_ui_off);

    switch (s_screen) {
    case SCR_ORIENT:
        draw_orient();
        break;
    case SCR_BOOT:
        draw_boot();
        break;
    case SCR_PLAYER:
        draw_player();
        break;
    case SCR_MENU:
        draw_menu();
        break;
    case SCR_LIST:
        draw_list();
        break;
    default:
        break;
    }

    tft_set_offset_y(0);
}

/* 切界面统一走这里：新界面从下方 18px 处滑上来 */
static void switch_to(screen_t s)
{
    s_screen = s;
    s_ui_off = 18;
    redraw();
}

/* ================================================================= */
/* 播放控制                                                           */
/* ================================================================= */
static void play_index(int i)
{
    if (i < 0 || i >= s_count) {
        return;
    }
    s_cur = i;
    set_title(s_files[i].name);

    char path[300];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", s_files[i].name);

    audio_stop();
    esp_err_t err = audio_play(path, s_files[i].duration * 1000u);
    ESP_LOGI(TAG, "播放 #%d %s -> %s", i, path, esp_err_to_name(err));

    if (err != ESP_OK) {
        /* 播放命令未入队（如队列已满）时不要标记为正在播放，
           否则自动切歌逻辑会反复触发。 */
        s_playing = false;
        return;
    }

    s_playing = true;
    s_play_at = now_ms();
    s_marquee = 0;
    s_title_alpha = 0;        /* 歌名淡入，避免硬切 */

    if (!s_asleep && s_screen == SCR_PLAYER) {
        draw_title();
        draw_progress();
        draw_times();
    }
}

static void track_step(int dir, bool user)
{
    if (s_count == 0) {
        return;
    }

    int next;
    if (g_set.mode == 1 && !user) {
        next = s_cur < 0 ? 0 : s_cur;                  /* 单曲循环 */
    } else if (g_set.mode == 2) {
        next = (int)(esp_random() % (uint32_t)s_count); /* 随机 */
    } else {
        next = (s_cur < 0 ? 0 : s_cur) + dir;
        if (next >= s_count) {
            next = 0;
        }
        if (next < 0) {
            next = s_count - 1;
        }
    }
    play_index(next);
}

static void toggle_play(void)
{
    if (s_count == 0) {
        return;
    }
    if (!s_playing || audio_state() == AUDIO_IDLE) {
        play_index(s_cur < 0 ? 0 : s_cur);
        return;
    }
    if (audio_state() == AUDIO_PLAYING) {
        audio_pause();
    } else if (audio_state() == AUDIO_PAUSED) {
        audio_resume();
    }
}

static void vol_step(int d)
{
    int v = (int)g_set.vol + d;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    if (v == (int)g_set.vol) {
        return;
    }
    g_set.vol = (uint8_t)v;
    audio_set_volume(g_set.vol);
    s_vol_dirty = now_ms();     /* 长按连发时不要每次写 NVS，停手后再存 */
    /* 不在这里直接画：player_tick 里做平滑插值 */
}

static void cycle_mode(void)
{
    g_set.mode = (uint8_t)((g_set.mode + 1) % 3);
    settings_save();
    draw_statusbar();
}

static void menu_adjust(void)
{
    switch (s_sel) {
    case 0:
        g_set.online        = !g_set.online;
        s_toggle_target[0]  = g_set.online != 0;
        /* 真的断开 / 重连，不然关了在线模式 IP 还挂着 */
        wifi_prov_set_online(g_set.online != 0);
        break;
    case 1:
        g_set.dark          = !g_set.dark;
        s_toggle_target[1]  = g_set.dark != 0;
        theme_update();
        break;
    case 2:
        g_set.bootanim      = !g_set.bootanim;
        s_toggle_target[2]  = g_set.bootanim != 0;
        break;
    case 3:
        g_set.bright = (uint8_t)((g_set.bright + 10) % 110);
        if (g_set.bright > 100) {
            g_set.bright = 0;
        }
        tft_set_brightness(g_set.bright);
        break;
    case 4:
        g_set.screenoff = (uint8_t)((g_set.screenoff + 1) % 4);
        break;
    default:
        return;                       /* 只读项 */
    }
    settings_save();
    draw_menu();
}

/* ================================================================= */
/* 按键                                                               */
/* ================================================================= */
static void list_move(int d)
{
    if (s_count == 0) {
        return;
    }
    s_sel      = (s_sel + d + s_count) % s_count;
    /* 内容已按新的 s_sel 上/下移一行，因此先用相反方向的偏移抵消，
       再由动画回归 0。若误写成 -d*28，会多移一行，表现为画面「向上抽一下」。 */
    s_list_off = d * LIST_ROW_H;
    draw_list_ex(false);
}

static void list_anim_tick(void)
{
    if (s_list_off == 0) {
        return;
    }
    /* 3px 一步 + 12ms 一帧：比 4px/14ms 更细，滚动看着才连续 */
    if (s_list_off > 0) {
        s_list_off -= 3;
        if (s_list_off < 0) {
            s_list_off = 0;
        }
    } else {
        s_list_off += 3;
        if (s_list_off > 0) {
            s_list_off = 0;
        }
    }

    if (s_list_off == 0) {
        draw_list_ex(true);      /* 收尾整屏重绘一次，清掉行滑出时的残影 */
    } else {
        draw_list_ex(false);
    }
}

static void handle_key(key_event_t ev)
{
    switch (s_screen) {
    case SCR_ORIENT:
        switch (ev) {
        case KEY_UP:
            s_orient = (s_orient + 3) % 4;
            tft_set_rotation(s_orient);
            redraw();
            break;
        case KEY_DOWN:
            s_orient = (s_orient + 1) % 4;
            tft_set_rotation(s_orient);
            redraw();
            break;
        case KEY_STAR:
            g_set.rot = (uint8_t)s_orient;
            settings_save();
            s_boot_at = now_ms();
            switch_to(g_set.bootanim ? SCR_BOOT : SCR_PLAYER);
            break;
        default:
            break;
        }
        break;

    case SCR_PLAYER:
        switch (ev) {
        case KEY_UP:
            track_step(-1, true);
            break;
        case KEY_DOWN:
            track_step(1, true);
            break;
        case KEY_UP_REPEAT:
            vol_step(5);
            break;
        case KEY_DOWN_REPEAT:
            vol_step(-5);
            break;
        case KEY_STAR:
            toggle_play();
            break;
        case KEY_STAR_LONG:
            cycle_mode();
            break;
        case KEY_STAR_DOUBLE:
            s_sel      = s_cur < 0 ? 0 : s_cur;
            s_list_top = 0;
            list_fix_top();
            switch_to(SCR_LIST);
            break;
        case KEY_HASH_DOUBLE:
            s_sel      = 0;
            s_menu_hi  = 28;
            s_menu_target = 28;
            /* 进入菜单时将开关拨片直接同步到当前设置，不播放过渡动画 */
            s_toggle_target[0] = g_set.online != 0;
            s_toggle_target[1] = g_set.dark != 0;
            s_toggle_target[2] = g_set.bootanim != 0;
            for (int i = 0; i < 3; i++) {
                s_toggle_pos[i] = s_toggle_target[i] ? 1.0f : 0.0f;
            }
            /* 菜单里用 * 调整选项，关掉它的双击，让每次按都立即生效 */
            keys_set_double(BTN_STAR, false);
            s_last_wifi_st = (int)wifi_prov_state();
            switch_to(SCR_MENU);
            break;
        case KEY_HASH_LONG:
            /* 长按 # 重新调屏幕方向（换方向后画面转 90° 时用） */
            s_orient = (g_set.rot <= 3) ? g_set.rot : 0;
            tft_set_rotation(s_orient);
            switch_to(SCR_ORIENT);
            break;
        default:
            break;
        }
        break;

    case SCR_MENU:
        switch (ev) {
        case KEY_UP:
            menu_move(-1);
            break;
        case KEY_DOWN:
            menu_move(1);
            break;
        case KEY_STAR:
            /* 菜单里 * = 调整当前项（取消双击后立即生效，连按不会退回播放器） */
            menu_adjust();
            break;
        case KEY_HASH_DOUBLE:
            s_sel    = 0;
            keys_set_double(BTN_STAR, true);   /* 回到播放界面恢复 * 的双击 */
            switch_to(SCR_PLAYER);
            break;
        default:
            break;
        }
        break;

    case SCR_LIST:
        switch (ev) {
        case KEY_UP:
        case KEY_UP_REPEAT:
            list_move(-1);
            break;
        case KEY_DOWN:
        case KEY_DOWN_REPEAT:
            list_move(1);
            break;
        case KEY_STAR:
            if (s_count > 0) {
                play_index(s_sel);
                switch_to(SCR_PLAYER);
            }
            break;
        case KEY_HASH_DOUBLE:
            switch_to(SCR_PLAYER);
            break;
        default:
            break;
        }
        break;

    default:
        break;
    }
}

/* ================================================================= */
/* 周期任务                                                           */
/* ================================================================= */
/* 播放结束 → 按播放模式自动接下一首。
   该检查必须在 ui_tick() 中、息屏判断之前执行：
   若放在 player_tick() 内，息屏时 ui_tick() 会提前返回，
   导致息屏后无法自动切歌与循环。 */
static int s_fail_count;      /* 连续未能开始播放的次数 */
static void check_track_end(int64_t now)
{
    if (!s_playing) {
        return;
    }

    audio_state_t st = audio_state();

    if (st == AUDIO_PLAYING) {
        s_fail_count = 0;     /* 已正常播放，重置失败计数 */
        return;
    }
    if (st != AUDIO_IDLE) {
        return;               /* 暂停状态下不切歌 */
    }

    if ((now - s_play_at) <= 3000) {
        return;               /* 刚切换过去，留出解码启动时间 */
    }

    if (s_count == 0) {
        s_playing = false;
        return;
    }

    /* 连续三次未能进入 PLAYING（文件无法打开、卡被拔出等）时停止重试，
       否则会每 3 秒切换一次并持续输出日志。 */
    if (s_fail_count >= 3) {
        ESP_LOGW(TAG, "连续 %d 次没能开始播放，停止自动切歌", s_fail_count);
        s_playing = false;
        return;
    }

    s_fail_count++;
    ESP_LOGI(TAG, "自动接下一首（第 %d 次）", s_fail_count);
    track_step(1, false);
}

static void player_tick(int64_t now)
{
    if (s_vol_dirty && (now - s_vol_dirty) > 800) {
        s_vol_dirty = 0;
        settings_save();
    }

    if (s_title_alpha < 100) {
        s_title_alpha += 18;
        if (s_title_alpha > 100) {
            s_title_alpha = 100;
        }
        draw_title();
    }

    /* 音量条平滑逼近目标值 */
    if (fabsf(s_vol_disp - (float)g_set.vol) > 0.4f) {
        s_vol_disp += ((float)g_set.vol - s_vol_disp) * 0.35f;
        if (fabsf(s_vol_disp - (float)g_set.vol) <= 0.4f) {
            s_vol_disp = (float)g_set.vol;
        }
        draw_volume();
    }

    if (now - s_last_prog > 200) {
        s_last_prog = now;
        draw_progress();
    }

    if (now - s_last_time > 1000) {
        s_last_time = now;
        draw_times();
    }

    if (s_title_w > 216 && (now - s_last_marq) > 60) {
        s_last_marq = now;
        s_marquee += 2;
        if (s_marquee > s_title_w + 40) {
            s_marquee = 0;
        }
        draw_title();
    }

    if (audio_state() == AUDIO_PLAYING && (now - s_last_disc) > 80) {
        s_last_disc  = now;
        s_disc_angle = (s_disc_angle + 12) % 360;
        draw_disc();
    }
}

void ui_init(void)
{
    keys_init();          /* 必须先配 GPIO，否则按键永远读不到 */
    s_vol_disp = (float)g_set.vol;

    /* 启动时按在线模式设置决定连不连路由器 */
    wifi_prov_set_online(g_set.online != 0);

    theme_update();
    tft_set_brightness(g_set.bright);

    s_sd_ready = sd_card_ready();
    s_count    = s_sd_ready ? sd_card_list_mp3(s_files, 100) : 0;
    ESP_LOGI(TAG, "UI 初始化：SD %s，%d 首", s_sd_ready ? "就绪" : "未检测到", s_count);

    if (s_count > 0) {
        s_cur = 0;
        set_title(s_files[0].name);
    } else {
        strcpy(s_title, "没有歌曲");
        s_title_w = font_text_width(s_title);
    }

    /* 方向已经定下来了，开机直接用存的值；要重调长按 # */
    tft_set_rotation(g_set.rot <= 3 ? g_set.rot : 0);
    s_screen = g_set.bootanim ? SCR_BOOT : SCR_PLAYER;
    s_boot_at       = now_ms();
    s_last_activity = s_boot_at;
    s_last_prog     = s_boot_at;
    s_last_time     = s_boot_at;
    s_last_marq     = s_boot_at;
    s_last_disc     = s_boot_at;

    redraw();
    tft_flush();
}

void ui_tick(void)
{
    int64_t now = now_ms();

    keys_poll();

    /* 自动接下一首需在息屏判断之前执行，保证息屏时仍能连续播放 */
    check_track_end(now);

    /* 息屏时任意键只负责唤醒，不执行功能 */
    if (s_asleep) {
        if (buttons_read() != BTN_NONE) {
            s_asleep        = false;
            tft_wake();
            keys_flush();
            s_last_activity = now;
            redraw();
            tft_flush();
        }
        return;
    }

    key_event_t ev;
    while (keys_next(&ev)) {
        s_last_activity = now;
        handle_key(ev);
    }

    /* 界面滑入过渡：新界面从下方 18px 处逐帧归位 */
    if (s_ui_off > 0) {
        s_ui_off -= 6;
        if (s_ui_off < 0) {
            s_ui_off = 0;
        }
        redraw();
    } else if (s_screen == SCR_BOOT) {
        if (now - s_boot_at > 1400) {
            s_screen = SCR_PLAYER;
            redraw();
        }
    } else if (s_screen == SCR_PLAYER) {
        player_tick(now);
    } else if (s_screen == SCR_MENU) {
        if ((now - s_last_menu_anim) > 20) {
            s_last_menu_anim = now;
            menu_anim_tick();
        }
        /* 每秒看一眼 WiFi 状态：连上/断开时自动刷新 IP 行 */
        if ((now - s_last_ip_check) > 1000) {
            s_last_ip_check = now;
            int st = (int)wifi_prov_state();
            if (st != s_last_wifi_st) {
                s_last_wifi_st = st;
                refresh_ip_row();
            }
        }
    } else if (s_screen == SCR_LIST) {
        if (s_list_off != 0 && (now - s_last_list_anim) > 12) {
            s_last_list_anim = now;
            list_anim_tick();
        }
    }

    /* 这一帧攒下的所有改动，整块推给屏幕 */
    tft_flush();

    if (now - s_last_activity > (int64_t)settings_screenoff_sec() * 1000) {
        tft_sleep();
        s_asleep = true;
        keys_flush();
    }
}
