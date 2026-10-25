/*
 * font_data —— 4bpp 字库访问实现
 */

#include "font_data.h"

#include <stddef.h>

/* EMBED_FILES 生成的符号（font_cn.bin → _binary_font_cn_bin_start） */
extern const uint8_t _binary_font_cn_bin_start[]    asm("_binary_font_cn_bin_start");
extern const uint8_t _binary_font_ascii_bin_start[] asm("_binary_font_ascii_bin_start");

const uint8_t *font_ascii(int c)
{
    if (c < 32 || c > 126) {
        return NULL;
    }
    return _binary_font_ascii_bin_start + (size_t)(c - 32) * FONT_GLYPH_BYTES;
}

const uint8_t *font_lookup(uint32_t cp)
{
    int lo = 0;
    int hi = g_cn_codepoints_count - 1;

    while (lo <= hi) {
        int      mid = (lo + hi) / 2;
        uint16_t v   = g_cn_codepoints[mid];
        if (v == cp) {
            return _binary_font_cn_bin_start + (size_t)mid * FONT_GLYPH_BYTES;
        }
        if (v < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

uint32_t font_utf8_next(const char *s, int *len)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char        c = *p;

    if (c < 0x80) {
        *len = 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        *len = 2;
        return ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0) {
        *len = 3;
        return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
               (p[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0) {
        *len = 4;
        return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
               ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    /* 非法首字节：跳 1 字节，画成缺字符 */
    *len = 1;
    return 0xFFFD;
}

int font_text_width(const char *s)
{
    int w = 0;
    for (const char *p = s; *p;) {
        int      n;
        uint32_t cp = font_utf8_next(p, &n);
        w += (cp < 0x80) ? FONT_ASCII_W : FONT_W;
        p += n;
    }
    return w;
}
