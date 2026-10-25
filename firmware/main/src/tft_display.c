/*
 * tft_display —— ST7789 240x240 IPS 实现
 *
 * 文字：4bpp 抗锯齿字库（font_data.c），按行合成到缓冲再一次刷屏。
 */

#include "tft_display.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "font_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "tft";

/* 引脚分配（避开 I2S 的 4/5/6 和 SD 的 10~13） */
#define PIN_SCLK  15
#define PIN_MOSI  16
#define PIN_RST   17
#define PIN_DC    18
#define PIN_BL    21

/* SPI 时钟。
 * 全屏 115KB：10MHz 要 90ms，40MHz 23ms，80MHz 11ms。
 * 杜邦线能扛到多少是运气，花屏/黑屏就往下退：80 → 40 → 26 → 20。 */
#define SPI_CLK_HZ  (80 * 1000 * 1000)

/* 填充用的多行缓冲：一次传 32 行，比逐行发少 32 倍的传输开销 */
#define FILL_ROWS   32

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t      s_flush_done;

static int  s_bright = 70;
static bool s_bl_on  = true;

static uint16_t s_textbuf[TFT_WIDTH * FONT_H];

/* 整帧缓冲：所有绘制先写在这里，由 tft_flush() 一次性推送。
 * 若改为逐块绘制并发送，画面会从顶部向下分段出现。 */
static uint16_t *s_fb;
static int       s_dirty_y0 = TFT_HEIGHT;
static int       s_dirty_y1 = 0;
static int       s_off_y;          /* 全局纵向偏移，用于界面滑入过渡 */

void tft_set_offset_y(int dy)
{
    s_off_y = dy;
}

static void mark_dirty(int y0, int y1)
{
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > TFT_HEIGHT) {
        y1 = TFT_HEIGHT;
    }
    if (y0 < s_dirty_y0) {
        s_dirty_y0 = y0;
    }
    if (y1 > s_dirty_y1) {
        s_dirty_y1 = y1;
    }
}

/* RGB565 高低字节交换。
 * ESP-IDF 的 SPI 屏驱动按原样发送 uint16_t（小端），而 ST7789 要求大端；
 * 若不交换，0xF79E（浅灰）会显示为 0x9EF7（青绿）。 */
static inline uint16_t to_panel(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

/* 背光走 LEDC PWM，亮度和开关分开控制 */
static void apply_backlight(void)
{
    uint32_t duty;

    if (!s_bl_on) {
        duty = 0;
    } else {
        /* 亮度最低保留 4%（约 10/255）：即使菜单调到 0 也不会全黑，
           否则屏幕会完全熄灭，无法再操作。 */
        duty = 10 + (uint32_t)s_bright * 245 / 100;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static bool on_color_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_flush_done) {
        xSemaphoreGiveFromISR(s_flush_done, &woken);
    }
    return woken == pdTRUE;
}

/* 等上一笔传输完成。esp_lcd 的 SPI 发送是异步的，不等就把缓冲改掉会撕裂。 */
static void wait_flush(void)
{
    if (s_flush_done) {
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
    }
}

esp_err_t tft_init(void)
{
    /* 背光先关，等初始化完再亮，避免上电闪白 */
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), TAG, "LEDC 定时器失败");

    ledc_channel_config_t ledc_ch = {
        .gpio_num   = PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ledc_ch), TAG, "LEDC 通道失败");

    s_flush_done = xSemaphoreCreateBinary();
    /* 先释放一次信号量：首次传输前调用 wait_flush() 时不至于等待超时 */
    if (s_flush_done) {
        xSemaphoreGive(s_flush_done);
    }

    /* 整帧缓冲放在内部 RAM：SPI 的 DMA 只能直接访问内部 RAM（PSRAM 不具备 DMA 能力）。
       240×240×2 = 115KB，ESP32-S3 的 512KB SRAM 可以容纳。 */
    s_fb = heap_caps_malloc((size_t)TFT_WIDTH * TFT_HEIGHT * sizeof(uint16_t),
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "帧缓冲分配失败（需要 115KB 内部 RAM）");
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, (size_t)TFT_WIDTH * TFT_HEIGHT * sizeof(uint16_t));

    spi_bus_config_t bus = {
        .sclk_io_num     = PIN_SCLK,
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = TFT_WIDTH * TFT_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "SPI3 初始化失败");

    /* 注意 cs_gpio_num = -1：这块屏没有 CS 脚 */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_DC,
        .cs_gpio_num       = -1,
        .pclk_hz           = SPI_CLK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        /* ST7789 有两种 SPI 时序：mode 0 和 mode 3。
           配错时初始化命令可能勉强能收到（屏幕会闪一下），
           但像素数据全乱 → 纯黑或花屏。先试 mode 3。 */
        .spi_mode          = 3,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_done,
        .user_ctx            = NULL,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_cfg, &io),
        TAG, "panel io 创建失败");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        /* 本屏为 BGR 顺序：若改为 RGB，红色会显示成蓝色。 */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel),
                        TAG, "ST7789 创建失败");

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);

    /* IPS 面板通常需要反色，否则黑白颠倒。
       若实际显示颜色发灰或偏色，可将 true 改为 false 再试。 */
    esp_lcd_panel_invert_color(s_panel, true);
    /* 本屏坐标是转置的：不做 swap 时，右上与左下的内容会互换。 */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_bl_on = true;
    apply_backlight();
    ESP_LOGI(TAG, "ST7789 240x240 就绪（SPI3, %dMHz）", SPI_CLK_HZ / 1000000);
    return ESP_OK;
}

void tft_fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!s_fb) {
        return;
    }
    y0 += s_off_y;
    y1 += s_off_y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > TFT_WIDTH)  x1 = TFT_WIDTH;
    if (y1 > TFT_HEIGHT) y1 = TFT_HEIGHT;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    uint16_t pc = to_panel(color);
    for (int y = y0; y < y1; y++) {
        uint16_t *row = &s_fb[(size_t)y * TFT_WIDTH];
        for (int x = x0; x < x1; x++) {
            row[x] = pc;
        }
    }
    mark_dirty(y0, y1);
}

void tft_fill(uint16_t color)
{
    if (!s_fb) {
        return;
    }
    /* 整屏清背景不受滑入偏移影响，否则会露出一条旧画面 */
    uint16_t pc = to_panel(color);
    for (int i = 0; i < TFT_WIDTH * TFT_HEIGHT; i++) {
        s_fb[i] = pc;
    }
    mark_dirty(0, TFT_HEIGHT);
}

void tft_blit(int x, int y, int w, int h, const uint16_t *buf)
{
    if (!s_fb || !buf || w <= 0 || h <= 0) {
        return;
    }
    y += s_off_y;
    if (x < 0 || y < 0 || x + w > TFT_WIDTH || y + h > TFT_HEIGHT) {
        return;
    }

    for (int ry = 0; ry < h; ry++) {
        uint16_t       *dst = &s_fb[(size_t)(y + ry) * TFT_WIDTH + x];
        const uint16_t *src = &buf[(size_t)ry * w];
        for (int rx = 0; rx < w; rx++) {
            dst[rx] = to_panel(src[rx]);
        }
    }
    mark_dirty(y, y + h);
}

/* 把本帧的脏区域一次性推送到屏幕，使画面整块更新。 */
void tft_flush(void)
{
    if (!s_panel || !s_fb) {
        return;
    }
    if (s_dirty_y0 >= s_dirty_y1) {
        return;
    }

    int y0 = s_dirty_y0;
    int y1 = s_dirty_y1;
    s_dirty_y0 = TFT_HEIGHT;
    s_dirty_y1 = 0;

    /* 115KB 的单次 DMA 传输在部分时序下不稳定，因此拆成两块发送；
       80MHz 下每块约 6ms，视觉上仍是连续刷新。 */
    const int BLOCK = 120;
    for (int y = y0; y < y1; y += BLOCK) {
        int rows = (y + BLOCK <= y1) ? BLOCK : (y1 - y);
        wait_flush();
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, TFT_WIDTH, y + rows,
                                  &s_fb[(size_t)y * TFT_WIDTH]);
    }
}

static int isqrt_i(int v)
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

/* 圆角矩形：在内存里一次性画好再发，避免逐行发几十次 SPI 事务。
 * 圆角外的像素填 bg（调用方传当前背景色）。 */
void tft_fill_round(int x, int y, int w, int h, int r, uint16_t color, uint16_t bg)
{
    static uint16_t buf[240 * 32];

    if (!s_panel || w <= 0 || h <= 0) {
        return;
    }
    if (w > 240 || h > 32 || r <= 0) {
        tft_fill_rect(x, y, x + w, y + h, color);
        return;
    }

    for (int ry = 0; ry < h; ry++) {
        int dx = 0;
        if (ry < r) {
            int dy = r - ry;
            dx = r - isqrt_i(r * r - dy * dy);
        } else if (ry >= h - r) {
            int dy = ry - (h - r - 1);
            dx = r - isqrt_i(r * r - dy * dy);
        }
        for (int rx = 0; rx < w; rx++) {
            buf[ry * w + rx] = (rx >= dx && rx < w - dx) ? color : bg;
        }
    }
    tft_blit(x, y, w, h, buf);
}

void tft_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    tft_fill_rect(x0, y0, x1, y0 + 2, color);          /* 上 */
    tft_fill_rect(x0, y1 - 2, x1, y1, color);          /* 下 */
    tft_fill_rect(x0, y0, x0 + 2, y1, color);          /* 左 */
    tft_fill_rect(x1 - 2, y0, x1, y1, color);          /* 右 */
}

/* ================================================================= */
/* 文字绘制                                                           */
/* ================================================================= */
static inline uint16_t blend565(uint16_t bg, uint16_t fg, unsigned a)
{
    /* ⚠️ 必须用有符号：浅色主题下文字比背景暗，(fr - br) 为负，
       用 unsigned 会下溢成巨大值，混合结果变成彩色噪点。 */
    int br = (bg >> 11) & 0x1F, bcg = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int fr = (fg >> 11) & 0x1F, fcg = (fg >> 5) & 0x3F, fb = fg & 0x1F;

    int r = br + ((fr - br) * (int)a) / 15;
    int g = bcg + ((fcg - bcg) * (int)a) / 15;
    int b = bb + ((fb - bb) * (int)a) / 15;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

int tft_text_width(const char *s)
{
    return s ? font_text_width(s) : 0;
}

int tft_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    if (!s_fb || !s) {
        return 0;
    }

    y += s_off_y;

    int total_w = font_text_width(s);
    if (total_w <= 0) {
        return 0;
    }

    /* 垂直方向只支持完整落在屏内：UI 布局保证这一点 */
    if (y < 0 || y + FONT_H > TFT_HEIGHT) {
        return total_w;
    }

    int vis_x0 = (x < 0) ? 0 : x;
    int vis_x1 = (x + total_w > TFT_WIDTH) ? TFT_WIDTH : x + total_w;
    int w      = vis_x1 - vis_x0;
    if (w <= 0) {
        return total_w;
    }

    /* 先铺背景 */
    uint16_t pbg = to_panel(bg);
    for (int i = 0; i < w * FONT_H; i++) {
        s_textbuf[i] = pbg;
    }

    int cx = x;
    for (const char *p = s; *p;) {
        int      n;
        uint32_t cp = font_utf8_next(p, &n);

        const uint8_t *g;
        int            gw;
        if (cp < 0x80) {
            g  = font_ascii((int)cp);
            gw = FONT_ASCII_W;
        } else {
            g  = font_lookup(cp);
            gw = FONT_W;
        }

        /* 只画落在可见区内的那几个像素列 */
        int gx0 = (cx < vis_x0) ? (vis_x0 - cx) : 0;
        int gx1 = (cx + gw > vis_x1) ? (vis_x1 - cx) : gw;
        int dx  = cx - vis_x0;

        for (int ry = 0; ry < FONT_H; ry++) {
            if (!g) {
                /* 字库没有这个字：空心方块，一眼能看出缺字 */
                if (gx0 <= 0 && gx1 >= gw && (ry == 0 || ry == FONT_H - 1)) {
                    for (int rx = 0; rx < gw; rx++) {
                        s_textbuf[ry * w + dx + rx] = to_panel(fg);
                    }
                } else if ((ry > 0 && ry < FONT_H - 1) && (gx0 <= 0 || gx1 >= gw)) {
                    int rx = (gx0 <= 0) ? 0 : gw - 1;
                    if (rx >= gx0 && rx < gx1) {
                        s_textbuf[ry * w + dx + rx] = to_panel(fg);
                    }
                }
                continue;
            }
            const uint8_t *row = g + ry * (FONT_W / 2);
            for (int rx = gx0; rx < gx1; rx++) {
                uint8_t  b = row[rx >> 1];
                unsigned a = (rx & 1) ? (b & 0x0F) : (b >> 4);
                if (a) {
                    s_textbuf[ry * w + dx + rx] = to_panel(blend565(bg, fg, a));
                }
            }
        }

        cx += gw;
        p  += n;
    }

    /* 合成好的这一行直接写进整帧缓冲，等这一帧结束再统一发 */
    for (int ry = 0; ry < FONT_H; ry++) {
        memcpy(&s_fb[(size_t)(y + ry) * TFT_WIDTH + vis_x0],
               &s_textbuf[(size_t)ry * w],
               (size_t)w * sizeof(uint16_t));
    }
    mark_dirty(y, y + FONT_H);
    return total_w;
}

void tft_set_orientation(int mode)
{
    if (!s_panel) {
        return;
    }
    bool mx = (mode & 1) != 0;
    bool my = (mode & 2) != 0;
    esp_lcd_panel_mirror(s_panel, mx, my);
}

void tft_set_rotation(int mode)
{
    if (!s_panel) {
        return;
    }

    bool swap = false, mx = false, my = false;
    switch (mode & 3) {
    case 0: swap = false; mx = false; my = false; break;   /* 0x00 横排 */
    case 1: swap = false; mx = true;  my = true;  break;   /* 0xC0 */
    case 2: swap = true;  mx = true;  my = false; break;   /* 0x60 */
    case 3: swap = true;  mx = false; my = true;  break;   /* 0xA0 */
    }

    wait_flush();
    esp_lcd_panel_swap_xy(s_panel, swap);
    esp_lcd_panel_mirror(s_panel, mx, my);
}

void tft_backlight(bool on)
{
    s_bl_on = on;
    apply_backlight();
}

void tft_set_brightness(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_bright = pct;
    apply_backlight();
}

void tft_sleep(void)
{
    if (!s_panel) {
        return;
    }

    /* ① 先清成纯黑：让所有像素回到同一状态。
          这一步是防图像残留的关键 —— 局部长期偏置才会留下残影。 */
    tft_fill(TFT_BLACK);
    tft_flush();        /* 必须真的发出去，不能只改内存 */

    /* ② 关闭背光，避免清屏过程出现可见闪烁 */
    s_bl_on = false;
    apply_backlight();

    /* ③ 面板进入 sleep：停止驱动液晶。
          只关背光的话面板还在刷新同一帧，液晶分子会持续疲劳。 */
    esp_lcd_panel_disp_sleep(s_panel, true);

    ESP_LOGI(TAG, "息屏（已清屏 + 面板休眠）");
}

void tft_wake(void)
{
    if (!s_panel) {
        return;
    }

    esp_lcd_panel_disp_sleep(s_panel, false);
    /* ST7789 从 sleep out 回到正常显示需要约 120ms，太早发数据会花屏 */
    vTaskDelay(pdMS_TO_TICKS(120));

    s_bl_on = true;
    apply_backlight();
    ESP_LOGI(TAG, "唤醒（界面需要重绘）");
}