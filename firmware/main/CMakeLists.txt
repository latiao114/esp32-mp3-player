idf_component_register(
    # 源文件在 src/，头文件在 include/（见本目录结构）
    SRCS         "src/main.c"
                 "src/wifi_prov.c"
                 "src/web_ui.c"
                 "src/music_api.c"
                 "src/tft_display.c"
                 "src/buttons.c"
                 "src/sd_card.c"
                 "src/audio_player.c"
                 "src/font_data.c"
                 "src/font_index.c"
                 "src/keys.c"
                 "src/ui.c"
                 "src/settings.c"
    INCLUDE_DIRS "include"
    # 两个页面都嵌进固件：
    #   index.html    连热点时看到（配网）
    #   download.html 连同一局域网时看到（音乐库）
    # 字库用二进制嵌入（4bpp 抗锯齿，中文全字库 1.6MB，别生成 .c 数组）
    EMBED_FILES  "www/index.html"
                 "www/download.html"
                 "font_cn.bin"
                 "font_ascii.bin"
    REQUIRES     esp_wifi
                 esp_netif
                 esp_event
                 nvs_flash
                 esp_http_server
                 esp_http_client
                 mbedtls
                 cjson
                 esp_timer
                 # 1.3 寸 ST7789 彩屏（esp_lcd 自带驱动）
                 esp_lcd
                 driver
                 # 背光 PWM（亮度调节）
                 esp_driver_ledc
                 # 读固件版本号（esp_app_get_description）
                 esp_app_format
                 # SD 卡（SPI 模式 + FAT 文件系统）
                 esp_driver_sdspi
                 fatfs
                 sdmmc
                 # I2S 音频输出
                 esp_driver_i2s
)
