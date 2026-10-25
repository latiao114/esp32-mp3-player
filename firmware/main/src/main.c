/*
 * ESP32-S3 音乐播放器
 *
 * 组成：
 *   1. AP 配网 + 局域网音乐库网页（web_ui / wifi_prov / music_api）
 *   2. 1.3 寸 ST7789 彩屏 + 4 键界面（tft_display / keys / ui / settings）
 *   3. SD 卡本地 MP3 + PCM5102A 输出（sd_card / audio_player）
 *
 * 主循环只有两件事：每 20ms 调 ui_tick()，其余全在各自任务里。
 */

#include <stdbool.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio_player.h"
#include "sd_card.h"
#include "settings.h"
#include "tft_display.h"
#include "ui.h"
#include "web_ui.h"
#include "wifi_prov.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "##########################################");
    ESP_LOGI(TAG, "#  ESP32-S3 音乐播放器                  #");
    ESP_LOGI(TAG, "#  固件版本 %-27s#", esp_app_get_description()->version);
    ESP_LOGI(TAG, "##########################################");

    /* NVS 必须先初始化，之后才能读取设置。
     * 若在 nvs_flash_init() 之前调用 settings_load()，nvs_open 会失败，
     * 设备将读不到任何已保存的配置，表现为"掉电后设置丢失"。 */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新格式化");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    settings_load();

    /* 在 WiFi 启动前把在线模式状态告知 wifi_prov。
       该标志决定 STA_START 时是否自动连接；若设置晚于 wifi_prov_start()，
       断开与自动连接会互相覆盖，导致开关失效。 */
    wifi_prov_set_online_flag(g_set.online != 0);

    /* 屏幕需在 WiFi 之前初始化：整帧缓冲占用 115KB 内部 RAM，
       WiFi 启动后这部分内存将无法分配。 */
    bool have_tft = (tft_init() == ESP_OK);
    if (!have_tft) {
        ESP_LOGW(TAG, "屏幕初始化失败，跳过显示（网页功能不受影响）");
    }

    ESP_ERROR_CHECK(wifi_prov_start());
    ESP_ERROR_CHECK(web_ui_start());

    if (sd_card_init() == ESP_OK) {
        ESP_LOGI(TAG, "SD 卡就绪");
    } else {
        ESP_LOGW(TAG, "SD 卡未挂载（没插卡 / 不是 FAT32 / 接线问题）");
    }

    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败，播放功能不可用");
    }

    if (have_tft) {
        ui_init();
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "──────────────── 怎么用 ────────────────");
    ESP_LOGI(TAG, "  手机 Wi-Fi 列表里找: %s", wifi_prov_ap_ssid());
    ESP_LOGI(TAG, "  连上后浏览器打开: http://192.168.4.1");
    ESP_LOGI(TAG, "  屏幕按键: ^上 v下 #井 *星");
    ESP_LOGI(TAG, "────────────────────────────────────────");

    while (true) {
        if (have_tft) {
            ui_tick();
        }
        /* 10ms 而不是 20ms：动画帧更密，按键响应也跟得上 */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
