/*
 * ui —— 屏幕界面与交互
 *
 * 三个界面（播放 / 菜单 / 选歌）+ 开机动画 + 息屏。
 * 设计依据：UI与交互设计.md
 *
 * 刷新策略：切换界面时整屏重绘；播放界面的滚动歌名、进度条、唱片局部刷新。
 * 屏幕是 10MHz SPI，整屏一次约 90ms，所以能局部刷就局部刷。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化界面（需先 tft_init、settings_load、sd_card_init） */
void ui_init(void);

/** @brief 主循环每 20ms 调一次：扫按键、跑动画、处理息屏 */
void ui_tick(void);

#ifdef __cplusplus
}
#endif
