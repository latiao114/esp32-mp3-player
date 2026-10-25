cmake_minimum_required(VERSION 3.16)

# 目标芯片固定 ESP32-S3（本工程依赖 S3 的 I2S + PSRAM）
set(IDF_TARGET "esp32s3")

# ── 测试版版本号 ────────────────────────────────────────────
# 现在还在测试阶段，版本固定 11.4.1。
# 正式版发布时只改这一行，菜单「固件」项和启动日志都会跟着变。
set(PROJECT_VER "11.4.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-mp3-player)
