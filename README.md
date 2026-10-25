# ESP32-S3 音乐播放器

基于 ESP32-S3 的桌面音乐播放器：1.3 寸彩屏 + 4 键操作，本地 SD 卡播放 MP3，
同时提供一个网页端的音乐库（支持在线搜索试听）。

## 功能

**本地播放**
- SD 卡 MP3 播放，PCM5102A I2S DAC 输出
- 歌名滚动、播放进度、音量条
- 顺序 / 单曲循环 / 随机三种播放模式
- 自动接续下一首（息屏时同样生效）

**屏幕界面**
- 240×240 ST7789 IPS 彩屏，20px 抗锯齿中文字库（GB2312 全字库）
- 播放 / 设置 / 选歌三个界面，带滑入过渡动画
- 菜单项带开关拨片动画、高亮滑动
- 息屏保护（防液晶残影），任意键唤醒
- 深色 / 浅色两套主题

**按键**

| 按键 | 播放界面 | 设置 | 选歌 |
|---|---|---|---|
| `^` | 单击上一首，长按音量+ | 上一项 | 上移 |
| `v` | 单击下一首，长按音量− | 下一项 | 下移 |
| `*` | 单击播放/暂停，长按切模式，双击选歌 | 调整当前项 | 选中播放 |
| `#` | 双击进设置，长按调屏幕方向 | 双击退出 | 双击退出 |

**网页端**
- AP 配网：手机连热点填写 WiFi 即可，配置存 NVS，掉电不丢
- 音乐库页面：浏览 SD 卡歌曲、在线搜索（iTunes API）、在线试听

## 硬件

| 模块 | 说明 |
|---|---|
| ESP32-S3 开发板 | 需要 PSRAM（N16R8），Flash 16MB |
| 1.3 寸 ST7789 | 240×240 IPS，4-SPI（模块无 CS 脚） |
| PCM5102A | I2S DAC，3.5mm 耳机输出 |
| MicroSD 模块 | SPI 接口 |
| 4 个轻触按键 | 也可用屏幕模块自带的按键 |

接线详见 [`docs/接线总表.md`](docs/接线总表.md)。

> ⚠️ 屏幕模块没有 CS 脚，必须独占一条 SPI 总线，不能和 SD 卡共用。

## 构建

需要 **ESP-IDF v6.x**（本项目在 v6.0.2 上验证）。

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

首次编译前注意：

- `sdkconfig.defaults` 已包含 PSRAM（Octal）与 16MB Flash 配置
- `main/` 下按 `src/`（源文件）、`include/`（头文件）、`www/`（网页）组织
- 字库以 `.bin` 形式通过 `EMBED_FILES` 嵌入，见下方「字体」

## 目录结构

```
firmware/
├── CMakeLists.txt
├── partitions.csv          # 16MB：4MB app + 12MB 存储分区
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── font_cn.bin         # 中文字库（思源黑体 20px，4bpp）
    ├── font_ascii.bin
    ├── include/            # 头文件
    ├── src/                # 源文件
    └── www/                # 网页（配网页 + 音乐库页）
docs/                       # 硬件与 UI 设计文档
```

## 字体

字库是把 **思源黑体（Noto Sans SC）** 用 20px 渲染成 4bpp 抗锯齿点阵，
覆盖 GB2312 全部 6763 个汉字，随固件嵌入。

思源黑体采用 [SIL Open Font License 1.1](https://scripts.sil.org/OFL)，
可自由分发与嵌入。

如需重新生成（换字号、换字体、增加字符）：

```bash
python tools/gen_font.py
```

脚本需要 Pillow，并自行准备字体文件。

## 许可

MIT License，详见 [LICENSE](LICENSE)。

第三方组件：
- [minimp3](https://github.com/lieff/minimp3) — CC0
- [cJSON](https://github.com/DaveGamble/cJSON) — MIT
- Noto Sans SC — SIL OFL 1.1
