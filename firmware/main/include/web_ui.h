/*
 * web_ui —— 配网 HTTP 服务器
 *
 * 路由：
 *   GET  /          配网页面（嵌入在固件里，不依赖外网）
 *   GET  /scan      扫描附近 WiFi，返回 JSON
 *   POST /connect   提交 ssid + password，保存并尝试连接
 *   GET  /status    当前状态 JSON，网页用它轮询结果
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 启动 HTTP 服务器（需在 wifi_prov_start() 之后调用） */
esp_err_t web_ui_start(void);

/** @brief 停止服务器 */
void web_ui_stop(void);

#ifdef __cplusplus
}
#endif
