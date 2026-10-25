/*
 * wifi_prov —— AP 配网状态机
 *
 * 工作方式（APSTA 双模）：
 *   启动后开启热点，手机连接热点并通过网页完成配网；
 *   收到配网请求后，热点【保持开启】，同时由 STA 连接家庭 WiFi。
 *   连接成功后保持 AP 一小段时间，便于手机查看结果，随后重启进入正常工作模式；
 *   连接失败则退回 AP 模式，等待用户重新配网。
 *
 * 采用 APSTA 而非先关 AP 再连接的原因：
 *   关闭热点会导致手机断网，无法看到“连接成功/失败”的结果，体验较差。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STATE_IDLE = 0,        /* 还没起来 */
    WIFI_STATE_AP,              /* 热点开着，等配网 */
    WIFI_STATE_CONNECTING,      /* 正在连家里的 WiFi */
    WIFI_STATE_CONNECTED,       /* 连上了 */
    WIFI_STATE_FAILED,          /* 连失败了，已退回 AP */
} wifi_state_t;

/** @brief 启动配网（开热点 + 注册事件） */
esp_err_t wifi_prov_start(void);

wifi_state_t wifi_prov_state(void);
/** 状态名，给网页和串口用："ap" / "connecting" / "connected" / "failed" */
const char *wifi_prov_state_name(void);
/** 连上后的 IP，没连上返回 "" */
const char *wifi_prov_ip(void);
/** 当前连着的路由器名称（SSID），没连上返回 "" */
const char *wifi_prov_connected_ssid(void);
/** 失败原因，给网页显示 */
const char *wifi_prov_reason(void);
/** 热点 SSID */
const char *wifi_prov_ap_ssid(void);

/**
 * @brief 扫描附近 WiFi
 * @param out     输出数组
 * @param max     数组容量
 * @return 实际扫到的数量，负数表示失败
 */
int wifi_prov_scan(wifi_ap_record_t *out, int max);

/**
 * @brief 尝试连接指定 WiFi（异步，状态会在事件里更新）
 */
esp_err_t wifi_prov_connect(const char *ssid, const char *password);

/** @brief 把配网结果存进 NVS，重启后自动连 */
esp_err_t wifi_prov_save(const char *ssid, const char *password);

/** @brief 从 NVS 读上次配的 WiFi，没有返回 false */
bool wifi_prov_load(char *ssid, size_t ssid_len, char *pwd, size_t pwd_len);

/**
 * @brief 在线模式开关：真正断开 / 重连路由器
 *
 * 关掉之后热点保留（否则手机连不上、没法重新配网），
 * 但 STA 会断开，IP 清空，界面显示 "--"。
 * 打开后重新连 NVS 里存的网络，连上才有 IP。
 */
void wifi_prov_set_online(bool online);

/** @brief 当前是否处于在线模式 */
bool wifi_prov_online(void);

/**
 * @brief 只设置在线模式标志，不碰 WiFi
 *
 * 必须在 wifi_prov_start() 之前调用：STA_START 事件里的自动连接
 * 要看到这个标志，否则会出现"先断开、后又被自动连上"。
 */
void wifi_prov_set_online_flag(bool online);

#ifdef __cplusplus
}
#endif
