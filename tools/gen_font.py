# -*- coding: utf-8 -*-
"""
生成 4bpp 抗锯齿点阵字库（20×24 格）。

- 中文：思源黑体（Noto Sans SC）20px，基线 y=21（字形落在 y=5..23）
- ASCII：同样使用思源黑体 20px，基线 y=21，占左侧 11px
- 亮度做 gamma 0.85 提一点，避免抗锯齿让笔画发灰发虚

字体使用 SIL Open Font License 的思源黑体，可随代码一同分发。

输出：
  firmware/main/font_cn.bin     GB2312 全部 6763 字
  firmware/main/font_ascii.bin  95 个可打印 ASCII
  firmware/main/src/font_index.c  codepoint 排序表（二分查找用）
"""
import os

from PIL import Image, ImageDraw, ImageFont

W, H = 20, 24
BYTES_PER_GLYPH = W * H // 2      # 240
CN_FONT = r"C:\Windows\Fonts\NotoSansSC-VF.ttf"
AS_FONT = r"C:\Windows\Fonts\NotoSansSC-VF.ttf"
CN_BASELINE = 21
AS_BASELINE = 21
AS_WIDTH = 11
GAMMA = 0.85

# 输出到仓库里的 firmware 目录（以脚本位置为基准，不依赖绝对路径）
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    os.pardir, "firmware"))


def render(ch, font, baseline, dx=0):
    img = Image.new("L", (W, H), 0)
    ImageDraw.Draw(img).text((dx, baseline), ch, font=font, fill=255, anchor="ls")
    return img


def to_4bpp(img):
    px = img.load()
    out = bytearray(BYTES_PER_GLYPH)
    stride = W // 2
    for y in range(H):
        for x in range(W):
            a = (px[x, y] / 255.0) ** GAMMA
            v = int(a * 15 + 0.5)
            if v:
                out[y * stride + x // 2] |= v << (4 if x % 2 == 0 else 0)
    return bytes(out)


def gb2312_chars():
    chars = []
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


def preview(chars, fonts, out_path):
    scale = 4
    pad = 4
    widths = [AS_WIDTH if ord(c) <= 127 else W for c in chars]
    canvas = Image.new("RGB", (sum(widths) + pad * 2, H + pad * 2), (26, 26, 28))
    x = pad
    for ch, w in zip(chars, widths):
        if ord(ch) <= 127:
            img = render(ch, fonts["ascii"], AS_BASELINE)
        else:
            img = render(ch, fonts["cn"], CN_BASELINE)
        px = img.load()
        for yy in range(H):
            for xx in range(w):
                a = int((px[xx, yy] / 255.0) ** GAMMA * 15 + 0.5)
                if a:
                    bg = canvas.getpixel((x + xx, pad + yy))
                    fg = (235, 235, 235)
                    canvas.putpixel((x + xx, pad + yy),
                                    tuple((bg[i] * (15 - a) + fg[i] * a) // 15
                                          for i in range(3)))
        x += w
    canvas.resize((canvas.width * scale, canvas.height * scale), Image.NEAREST).save(out_path)


def main():
    cn_font = ImageFont.truetype(CN_FONT, 20, index=0)
    as_font = ImageFont.truetype(AS_FONT, 20, index=0)

    preview("晴天 - 周杰伦  音量 70  ABC 0123",
            {"cn": cn_font, "ascii": as_font},
            os.path.join(ROOT, os.pardir, "..", "_work", "font_preview20.png"))

    ascii_data = bytearray()
    for c in range(32, 127):
        ascii_data += to_4bpp(render(chr(c), as_font, AS_BASELINE))
    with open(os.path.join(ROOT, "main", "font_ascii.bin"), "wb") as f:
        f.write(ascii_data)

    chars = gb2312_chars()
    glyphs = []
    cps = []
    for ch in chars:
        glyphs.append(to_4bpp(render(ch, cn_font, CN_BASELINE)))
        cps.append(ord(ch))

    # 字库按码位升序存放，和索引表一一对应（二分查找的下标就是字形序号）
    order = sorted(range(len(cps)), key=lambda i: cps[i])
    cn_data = bytearray()
    for i in order:
        cn_data += glyphs[i]
    with open(os.path.join(ROOT, "main", "font_cn.bin"), "wb") as f:
        f.write(cn_data)

    lines = [
        "/* 由 _work/gen_font.py 自动生成，请勿手动修改 */",
        '#include "font_data.h"',
        "",
        "/* GB2312 汉字码位（升序），下标 = font_cn.bin 里的字形序号 */",
        "const uint16_t g_cn_codepoints[%d] = {" % len(order),
    ]
    buf = []
    for k, i in enumerate(order):
        buf.append("0x%04X" % cps[i])
        if len(buf) == 12:
            lines.append("    " + ", ".join(buf) + ",")
            buf = []
    if buf:
        lines.append("    " + ", ".join(buf) + ",")
    lines.append("};")
    lines.append("const int g_cn_codepoints_count = %d;" % len(order))
    with open(os.path.join(ROOT, "main", "src", "font_index.c"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print("中文 %d 字 / %d 字节，ASCII 95 字 / %d 字节"
          % (len(chars), len(cn_data), len(ascii_data)))


if __name__ == "__main__":
    main()
