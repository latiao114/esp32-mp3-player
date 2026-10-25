/*
 * sd_card —— MicroSD 卡（SPI 模式）实现
 */

#include "sd_card.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define PIN_CS    10
#define PIN_MOSI  11
#define PIN_SCK   12
#define PIN_MISO  13

static const char *TAG = "sd";

static sdmmc_card_t *s_card;
static bool          s_ready;

esp_err_t sd_card_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .sclk_io_num     = PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "SPI2 初始化失败");

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        /* 不启用自动格式化：卡内可能存在用户数据，格式化不可逆。
           卡格式不正确时应由用户在电脑上处理。 */
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    /* 本卡在 40MHz 下挂载失败（sd.present=false），因此维持默认 20MHz。
       如需提速，需换用支持高速模式的卡，或改用 SDMMC 4-bit 模式（需重新接线）。 */
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = PIN_CS;

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡挂载失败: %s（没插卡？卡不是 FAT32？）", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "SD 卡挂载成功，挂载点 %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool sd_card_ready(void)
{
    return s_ready;
}

esp_err_t sd_card_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(SD_MOUNT_POINT, total_bytes, free_bytes);
}

/* ================================================================= */
/* 估算 MP3 时长                                                      */
/* ================================================================= */
/*
 * 思路：跳过 ID3v2 标签，找到第一个 MPEG 帧头，取出比特率，
 * 再用 (音频字节数 × 8) / 比特率 估算总时长。
 *
 * CBR（固定码率）文件很准；VBR 文件会偏几秒，但至少能显示个大概。
 * 要精确就得解析 Xing/Info 头里的总帧数，那是下一步的事。
 */
static uint32_t mp3_duration_sec(const char *path, long file_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }

    /* 跳过 ID3v2 标签（10 字节头 + 同步安全整数表示的长度） */
    long audio_start = 0;
    unsigned char id3[10];
    if (fread(id3, 1, 10, f) == 10 &&
        id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3') {
        long sz = ((long)(id3[6] & 0x7f) << 21) |
                  ((long)(id3[7] & 0x7f) << 14) |
                  ((long)(id3[8] & 0x7f) << 7) |
                  (long)(id3[9] & 0x7f);
        audio_start = 10 + sz;
    }
    if (audio_start >= file_size) {
        fclose(f);
        return 0;
    }
    fseek(f, audio_start, SEEK_SET);

    /* 比特率表（kbps），下标 = 帧头里的 4 位索引 */
    static const uint16_t br_mpeg1[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };
    static const uint16_t br_mpeg2[16] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
    };

    uint32_t duration = 0;
    unsigned char h[4];

    /* 最多扫 64KB：正常文件第一个帧头就在开头几字节，
       限制一下是防止 ID3 里塞了大封面导致一直读下去 */
    for (long i = 0; i < 65536 && fread(h, 1, 4, f) == 4; i++) {
        if (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) {
            int ver   = (h[1] >> 3) & 0x03;   /* 3=MPEG1 2=MPEG2 0=MPEG2.5 */
            int layer = (h[1] >> 1) & 0x03;   /* 1 = Layer III */
            int br_i  = (h[2] >> 4) & 0x0F;

            if (ver != 1 && layer == 1 && br_i != 0 && br_i != 15) {
                int kbps = (ver == 3) ? br_mpeg1[br_i] : br_mpeg2[br_i];
                if (kbps > 0) {
                    long audio_bytes = file_size - audio_start;
                    duration = (uint32_t)((audio_bytes * 8) / (kbps * 1000));
                    break;
                }
            }
        }
        fseek(f, -3, SEEK_CUR);   /* 逐字节滑动 */
    }

    fclose(f);
    return duration;
}

int sd_card_list_mp3(sd_file_t *files, int max)
{
    if (!s_ready || !files || max <= 0) {
        return 0;
    }

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "打不开根目录");
        return 0;
    }

    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < max) {
        const char *name = e->d_name;
        if (name[0] == '.') {
            continue;                       /* 跳过 . / .. / 隐藏文件 */
        }
        size_t len = strlen(name);
        if (len < 5 || strcasecmp(name + len - 4, ".mp3") != 0) {
            continue;                       /* 只要 .mp3 */
        }

        char full[512];   /* 目录前缀 + 文件名最长 255，留足 */
        snprintf(full, sizeof(full), SD_MOUNT_POINT "/%s", name);

        struct stat st;
        long size = (stat(full, &st) == 0) ? (long)st.st_size : 0;

        strncpy(files[n].name, name, sizeof(files[n].name) - 1);
        files[n].name[sizeof(files[n].name) - 1] = '\0';
        files[n].size     = (uint32_t)size;
        files[n].duration = (size > 0) ? mp3_duration_sec(full, size) : 0;
        n++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "找到 %d 个 MP3", n);
    return n;
}
