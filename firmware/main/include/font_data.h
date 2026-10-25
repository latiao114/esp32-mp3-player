/*
 * font_data —— 4bpp 抗锯齿点阵字库（自动生成，请勿手动修改）
 *
 *   中文：微软雅黑 20px，GB2312 全部 6763 字，20×24 格
 *   ASCII：Consolas 20px，95 个可打印字符，占左侧 11px
 *
 * 每个字形 240 字节（20 宽 × 24 高，4bpp，每行 10 字节，高 nibble 是左像素）。
 * 字形数据放在独立 .bin 里（EMBED_FILES 嵌入 flash），避免生成几百 KB 的 .c。
 *
 * 重新生成：_work/gen_font.py
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_W            20
#define FONT_H            24
#define FONT_ASCII_W      11
#define FONT_GLYPH_BYTES  (FONT_W * FONT_H / 2)   /* 240 */

/** @brief GB2312 汉字码位表（升序），下标即字形序号 */
extern const uint16_t g_cn_codepoints[];
extern const int      g_cn_codepoints_count;

/**
 * @brief 按 Unicode 码位取字形
 * @return 144 字节字形数据；字库没有该字时返回 NULL
 */
const uint8_t *font_lookup(uint32_t cp);

/** @brief 取 ASCII 字形（c 必须在 32~126） */
const uint8_t *font_ascii(int c);

/**
 * @brief 解码一个 UTF-8 字符
 * @param s    字符串指针
 * @param len  输出：该字符占几个字节
 * @return 码位；遇到非法字节返回 0xFFFD
 */
uint32_t font_utf8_next(const char *s, int *len);

/** @brief 一段文本占多少像素宽（不含行距） */
int font_text_width(const char *s);

#ifdef __cplusplus
}
#endif
