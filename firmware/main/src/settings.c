/*
 * settings —— NVS 设置存储
 */

#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS  = "ui";

settings_t g_set;

void settings_load(void)
{
    /* 默认值：浅色、在线模式关、开机动画开、音量 60、亮度 70 */
    memset(&g_set, 0, sizeof(g_set));
    g_set.dark      = 0;
    g_set.online    = 0;
    g_set.bootanim  = 1;
    g_set.vol       = 60;
    g_set.bright    = 70;
    g_set.mode      = 0;
    g_set.screenoff = 2;        /* 默认 1 分钟 */
    g_set.rot       = 255;      /* 未设置 → 开机进方向自检 */

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "没有已保存的设置，用默认值");
        return;
    }

    /* 读失败就保留默认值 —— 不要因为一个 key 缺失就把整组设置丢掉 */
    if (nvs_get_u8(h, "dark", &g_set.dark) != ESP_OK) g_set.dark = 0;
    if (nvs_get_u8(h, "online", &g_set.online) != ESP_OK) g_set.online = 0;
    if (nvs_get_u8(h, "bootanim", &g_set.bootanim) != ESP_OK) g_set.bootanim = 1;
    if (nvs_get_u8(h, "vol", &g_set.vol) != ESP_OK) g_set.vol = 60;
    if (nvs_get_u8(h, "bright", &g_set.bright) != ESP_OK) g_set.bright = 70;
    if (nvs_get_u8(h, "mode", &g_set.mode) != ESP_OK) g_set.mode = 0;
    if (nvs_get_u8(h, "screenoff", &g_set.screenoff) != ESP_OK) g_set.screenoff = 2;
    if (nvs_get_u8(h, "rot", &g_set.rot) != ESP_OK) g_set.rot = 255;

    /* 版本升级：旧版本默认息屏 30 秒，新版本改为 1 分钟。
       没有 ver 标记的旧数据覆盖一次；此后不再改动用户设置。 */
    uint8_t ver = 0;
    if (nvs_get_u8(h, "ver", &ver) != ESP_OK || ver < 2) {
        g_set.screenoff = 2;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "设置已加载：暗色=%d 在线=%d 音量=%d 模式=%d",
             g_set.dark, g_set.online, g_set.vol, g_set.mode);
}

void settings_save(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败: %s（设置只保留在内存里）", esp_err_to_name(err));
        return;
    }

    nvs_set_u8(h, "dark", g_set.dark);
    nvs_set_u8(h, "online", g_set.online);
    nvs_set_u8(h, "bootanim", g_set.bootanim);
    nvs_set_u8(h, "vol", g_set.vol);
    nvs_set_u8(h, "bright", g_set.bright);
    nvs_set_u8(h, "mode", g_set.mode);
    nvs_set_u8(h, "screenoff", g_set.screenoff);
    nvs_set_u8(h, "rot", g_set.rot);
    nvs_set_u8(h, "ver", 2);

    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "保存失败: %s", esp_err_to_name(err));
    }
}

int settings_screenoff_sec(void)
{
    static const int sec[] = { 15, 30, 60, 300 };
    int              i     = g_set.screenoff;
    if (i > 3) {
        i = 1;
    }
    return sec[i];
}
