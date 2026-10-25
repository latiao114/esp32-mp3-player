/*
 * tft_display —— 1.3 寸 ST7789 240x240 IPS 彩屏驱动
 *
 * 硬件（模块背面丝印）：
 *   驱动 IC : ST7789
 *   分辨率  : 240x240
 *   接口    : 4-SPI，但【没有 CS 脚】—— 片选在模块内部固定拉低
 *
 * ⚠️ 正因为没有 CS，这块屏【不能】和 SD 卡共用 SPI 总线：
 *    共用时 SD 卡通信的数据也会被屏收进去，直接花屏。
 *    所以这里单独用 SPI3_HOST，SD 卡继续用另一套。
 *
 * 接线：
 *   SCL(GPIO15)  SDA(GPIO16)  RES(GPIO17)  DC(GPIO18)  BLK(GPIO21)
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TFT_WIDTH   240
#define TFT_HEIGHT  240

/* RGB565 常用色 */
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_BLUE    0x001F
#define TFT_YELLOW  0xFFE0
#define TFT_CYAN    0x07FF
#define TFT_MAGENTA 0xF81F

/** @brief 初始化 SPI3 + ST7789，并点亮背光 */
esp_err_t tft_init(void);

/** @brief 整屏填充一个颜色 */
void tft_fill(uint16_t color);

/** @brief 填充矩形（x0,y0 含；x1,y1 不含） */
void tft_fill_rect(int x0, int y0, int x1, int y1, uint16_t color);

/**
 * @brief 直接贴一块 RGB565 位图（唱片、图标这类预渲染图用）
 * @param w,h  像素尺寸；buf 长度必须是 w*h
 */
void tft_blit(int x, int y, int w, int h, const uint16_t *buf);

/**
 * @brief 圆角矩形（内部一次成型，只进帧缓冲）
 * @param r   圆角半径
 * @param bg  圆角外要填的背景色
 */
void tft_fill_round(int x, int y, int w, int h, int r, uint16_t color, uint16_t bg);

/**
 * @brief 把这一帧的脏区域一次性推给屏幕
 *
 * 所有绘制函数只写内存里的整帧缓冲，不碰 SPI。UI 每帧末尾调用一次，
 * 这样屏幕上是"整块换掉"，不会从上往下一条条刷出来。
 */
void tft_flush(void);

/**
 * @brief 全局纵向绘制偏移（界面切换时做滑入过渡用）
 * @note tft_fill() 不受影响，它永远清整屏
 */
void tft_set_offset_y(int dy);

/** @brief 画一个 1px 矩形边框 */
void tft_rect(int x0, int y0, int x1, int y1, uint16_t color);

/**
 * @brief 绘制一行文字（中英文混排）
 * @param x,y  左上角
 * @param s    UTF-8 字符串
 * @param fg   文字颜色
 * @param bg   背景色
 * @return 实际占用的宽度（像素）
 *
 * 中文 16×16、ASCII 8×16。字库见 font_data.h。
 * 没收录的汉字会画成空心方块，方便看出缺哪个字。
 */
int tft_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);

/** @brief 只算宽度，不绘制 */
int tft_text_width(const char *s);

/**
 * @brief 切换屏幕方向（调试用）
 * @param mode 0~3，对应 mirror(x,y) 的四种组合：00 / 10 / 01 / 11
 *
 * swap_xy 已经固定为 true（四色块位置验证过），这里只调 mirror。
 * 找到文字方向正确的那一种后，把编号写死在 tft_init() 里。
 */
void tft_set_orientation(int mode);

/**
 * @brief 设置屏幕方向 0~3（写进 NVS 的那个值）
 *
 * 四种组合分别对应 MADCTL 的 0xA0 / 0xC0 / 0x60 / 0x00。
 * 具体哪种方向正确取决于屏模块与排线，可通过方向自检由用户选定并保存。
 */
void tft_set_rotation(int mode);

/** @brief 背光开关（PWM，亮度和开关是分开的） */
void tft_backlight(bool on);

/** @brief 设置背光亮度 0~100（掉电不保存，由 settings 负责） */
void tft_set_brightness(int pct);

/**
 * @brief 息屏：清黑 + 关背光 + 面板休眠
 *
 * ⚠️ 不能只关背光。便宜的 TFT 液晶长时间显示同一帧会产生图像残留
 * （image persistence），关灯后面板仍在刷新，液晶持续处于局部偏置状态。
 * 这里先填纯黑让所有像素回到同一状态，再让面板真正休眠。
 */
void tft_sleep(void);

/**
 * @brief 唤醒：面板唤醒 + 开背光
 * @note 调用之后必须重绘当前界面（息屏时画面已经被清黑了）
 */
void tft_wake(void);

#ifdef __cplusplus
}
#endif
