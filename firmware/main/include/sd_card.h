/*
 * sd_card —— MicroSD 卡（SPI 模式）
 *
 * 接线：
 *   CS   → GPIO10
 *   MOSI → GPIO11
 *   SCK  → GPIO12
 *   MISO → GPIO13
 *
 * ⚠️ 用 SPI2，和屏幕的 SPI3 分开。
 *    屏幕模块没有 CS 脚（片选内部固定拉低），和 SD 共用总线会互相干扰。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT_POINT "/sdcard"

/** @brief 挂载 SD 卡（失败不影响其他功能，只是没有本地音乐） */
esp_err_t sd_card_init(void);

/** @brief 卡是否挂载成功 */
bool sd_card_ready(void);

/** @brief 取容量信息（字节），未挂载返回错误 */
esp_err_t sd_card_info(uint64_t *total_bytes, uint64_t *free_bytes);

/**
 * @brief 一个 MP3 文件的元信息
 */
typedef struct {
    char     name[64];
    uint32_t size;      /**< 文件大小（字节） */
    uint32_t duration;  /**< 时长（秒）。解析失败为 0 */
} sd_file_t;

/**
 * @brief 列出根目录下的 MP3 文件（含大小和时长）
 * @param files  输出数组
 * @param max    最多列几个
 * @return 实际找到的数量
 *
 * @note 时长是通过读取 MPEG 帧头里的比特率、再用文件大小估算的：
 *       CBR（固定码率）文件准确，VBR 会有几秒误差。
 */
int sd_card_list_mp3(sd_file_t *files, int max);

#ifdef __cplusplus
}
#endif
