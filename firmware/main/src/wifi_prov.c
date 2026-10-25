/*
 * wifi_prov —— AP 配网实现
 */

#include "wifi_prov.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_prov";

#define AP_SSID_PREFIX      "ESP32-MP3-"
#define CONNECT_TIMEOUT_MS  20000
#define NVS_NS              "wifi"

/* 断线后自动重连：每 3 秒尝试一次，共若干次，避免单次失败后不再重试。
   密码错误等原因重试无效，将直接上报错误，提示用户修改配置。 */
#define RECONNECT_DELAY_MS  3000
#define RECONNECT_MAX       10

static wifi_state_t         s_state = WIFI_STATE_IDLE;
static char                 s_ip[16];
static char                 s_reason[64];
static char                 s_ap_ssid[33];
static esp_timer_handle_t   s_timeout;
static esp_timer_handle_t   s_retry_timer;
/* 记住要连的是谁，重连时不用再去问 NVS */
static char                 s_sta_ssid[33];
static int                  s_retry;
static bool                 s_online = true;   /* 在线模式：关闭时主动断开 STA */
/* 扫描期间 STA 会短暂断开，这段时间内的断开事件要忽略 */
static volatile bool        s_scanning;

/* ================================================================= */
/* 状态                                                              */
/* ================================================================= */
wifi_state_t wifi_prov_state(void)          { return s_state; }
const char  *wifi_prov_ip(void)             { return s_ip; }
const char  *wifi_prov_reason(void)         { return s_reason; }
const char  *wifi_prov_ap_ssid(void)        { return s_ap_ssid; }

const char *wifi_prov_connected_ssid(void)
{
    /* 仅在已连接时有意义：直接向 WiFi 驱动查询，不自行维护状态副本 */
    static char s_sta_ssid[33];
    if (s_state != WIFI_STATE_CONNECTED) {
        return "";
    }
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        strncpy(s_sta_ssid, (const char *)info.ssid, sizeof(s_sta_ssid) - 1);
        s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
        return s_sta_ssid;
    }
    return "";
}

const char *wifi_prov_state_name(void)
{
    switch (s_state) {
    case WIFI_STATE_AP:         return "ap";
    case WIFI_STATE_CONNECTING: return "connecting";
    case WIFI_STATE_CONNECTED:  return "connected";
    case WIFI_STATE_FAILED:     return "failed";
    default:                    return "idle";
    }
}

static void set_failed(const char *why)
{
    s_state = WIFI_STATE_FAILED;
    snprintf(s_reason, sizeof(s_reason), "%s", why ? why : "连接失败");
    s_ip[0] = '\0';
    ESP_LOGW(TAG, "连接失败: %s（热点保持开启，可重新配网）", s_reason);
}

/* 将断开原因转换为可读文本，供网页直接展示 */
static const char *reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:              return "找不到这个网络，检查名称";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:        return "密码错误";
    case WIFI_REASON_ASSOC_FAIL:               return "路由器拒绝接入";
    case WIFI_REASON_CONNECTION_FAIL:          return "信号太弱或路由器不可达";
    default:                                   return "连接失败";
    }
}

/* 密码错误、握手失败等原因重试无效，需由用户修改配置 */
static bool is_auth_failure(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

/* 安排一次延时重连。
   这里刻意【不】把状态置为 failed：一旦标记为 failed，网页会显示
   "连接失败，请重新配网"，而设备实际仍在后台重试。
   该提示会让用户误以为 Wi-Fi 配置丢失，实际上 NVS 中的配置始终完好。 */
static void schedule_retry(const char *why)
{
    if (s_retry >= RECONNECT_MAX) {
        set_failed(why ? why : "连接失败");
        return;
    }
    s_retry++;
    s_state = WIFI_STATE_CONNECTING;
    s_ip[0] = '\0';
    snprintf(s_reason, sizeof(s_reason), "%s（第 %d 次重试）",
             why ? why : "断线", s_retry);

    ESP_LOGW(TAG, "%d 秒后重连（第 %d/%d 次）：%s",
             RECONNECT_DELAY_MS / 1000, s_retry, RECONNECT_MAX, why ? why : "");

    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)RECONNECT_DELAY_MS * 1000ULL);
    }
}

static void on_retry_timer(void *arg)
{
    (void)arg;
    if (s_state == WIFI_STATE_CONNECTED || s_sta_ssid[0] == '\0') {
        return;
    }
    ESP_LOGI(TAG, "重连 [%s]（第 %d 次）", s_sta_ssid, s_retry);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "重连调用失败: %s", esp_err_to_name(err));
    }

    if (s_timeout) {
        esp_timer_stop(s_timeout);
        esp_timer_start_once(s_timeout, (uint64_t)CONNECT_TIMEOUT_MS * 1000ULL);
    }
}

/* ================================================================= */
/* NVS                                                               */
/* ================================================================= */
esp_err_t wifi_prov_save(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", password);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "已保存 WiFi 配置: %s", ssid);
    return err;
}

bool wifi_prov_load(char *ssid, size_t ssid_len, char *pwd, size_t pwd_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = ssid_len;
    bool ok = (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK) && ssid[0] != '\0';
    if (ok) {
        len = pwd_len;
        if (nvs_get_str(h, "pass", pwd, &len) != ESP_OK) {
            pwd[0] = '\0';
        }
    }
    nvs_close(h);
    return ok;
}

/* ================================================================= */
/* 事件                                                              */
/* ================================================================= */
static void on_connect_timeout(void *arg)
{
    (void)arg;
    if (s_state == WIFI_STATE_CONNECTING) {
        /* 不直接判为失败：连接超时后交由重连逻辑继续尝试，
           避免 20 秒未连上就永久失败、只能重新配网。 */
        schedule_retry("连接超时");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "热点已开启: %s", s_ap_ssid);
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "★ 客户端连上热点：MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "热点客户端断开");
            break;
        case WIFI_EVENT_STA_START: {
            /* STA 在此时才真正就绪，必须等到该事件后再连接：
               在 esp_wifi_start() 之后立即调用 esp_wifi_connect() 会失败，
               因为此时 STA 尚未就绪。 */
            char ssid[33] = { 0 };
            char pwd[65]  = { 0 };
            if (!s_online) {
                ESP_LOGI(TAG, "STA 就绪，但在线模式是关的，不自动连接");
            } else if (wifi_prov_load(ssid, sizeof(ssid), pwd, sizeof(pwd))) {
                ESP_LOGI(TAG, "STA 就绪，自动连接已保存的网络 [%s]", ssid);
                wifi_prov_connect(ssid, pwd);
            } else {
                ESP_LOGI(TAG, "STA 就绪，但没有保存过网络，等待配网");
            }
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已关联到路由器，等待分配 IP…");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *ev = data;
            ESP_LOGI(TAG, "STA 断开, reason=%d", ev->reason);

            /* ── 以下两类断开事件需要忽略，否则会出现「配网成功却显示失败」 ──
             *
             * 1) ASSOC_LEAVE：由本程序调用 esp_wifi_disconnect() 主动断开时触发。
             *    该事件为异步投递，可能在 GOT_IP 之后才被处理；
             *    若不忽略，会把刚建立的成功状态覆盖为 FAILED。
             *
             * 2) 扫描期间：ESP32 扫描需要切换信道，STA 会短暂断开后自动连回，
             *    这同样不属于连接失败。
             */
            if (ev->reason == WIFI_REASON_ASSOC_LEAVE || s_scanning || !s_online) {
                break;
            }
            if (s_state != WIFI_STATE_CONNECTING && s_state != WIFI_STATE_CONNECTED) {
                break;
            }

            if (is_auth_failure(ev->reason)) {
                /* 密码错误重试无效，直接上报，提示用户修改配置 */
                set_failed(reason_str(ev->reason));
            } else {
                /* 信号弱、路由器未开启、DHCP 未分配到 IP 等情况均可自行恢复，
                   因此自动重试，避免用户误以为配置丢失而重新配网。 */
                schedule_retry(reason_str(ev->reason));
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_state = WIFI_STATE_CONNECTED;
        s_reason[0] = '\0';
        s_retry = 0;                     /* 连上了，重试计数清零 */
        if (s_timeout) {
            esp_timer_stop(s_timeout);
        }
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
        }
        ESP_LOGI(TAG, "================ 配网成功 ================");
        ESP_LOGI(TAG, "  IP: %s", s_ip);
        ESP_LOGI(TAG, "==========================================");
    }
}

/* ================================================================= */
/* 扫描                                                              */
/* ================================================================= */
int wifi_prov_scan(wifi_ap_record_t *out, int max)
{
    wifi_scan_config_t cfg = { 0 };
    cfg.show_hidden = false;

    /* 阻塞式扫描。扫描期间热点会短暂中断，STA 断开事件也要忽略掉。 */
    s_scanning = true;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    s_scanning = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描启动失败: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) {
        return 0;
    }
    if (count > (uint16_t)max) {
        count = (uint16_t)max;
    }
    if (esp_wifi_scan_get_ap_records(&count, out) != ESP_OK) {
        return -1;
    }
    return (int)count;
}

/* ================================================================= */
/* 连接                                                              */
/* ================================================================= */
esp_err_t wifi_prov_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 由用户主动发起的配网操作视为开启在线模式 */
    s_online = true;

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    /* 记住目标，断线后能自己重连，不用再去翻 NVS。
       密码不用另存 —— STA 的配置已经由上面的 esp_wifi_set_config 记住了。 */
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
    s_retry = 0;

    s_state = WIFI_STATE_CONNECTING;
    s_ip[0] = '\0';
    s_reason[0] = '\0';

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "设置 STA 配置失败");

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "发起连接失败");

    if (s_timeout) {
        esp_timer_stop(s_timeout);
        esp_timer_start_once(s_timeout, (uint64_t)CONNECT_TIMEOUT_MS * 1000ULL);
    }

    ESP_LOGI(TAG, "正在连接 %s …", ssid);
    return ESP_OK;
}

/* ================================================================= */
/* 启动                                                              */
/* ================================================================= */
esp_err_t wifi_prov_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新格式化");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS 擦除失败");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS 初始化失败");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif 初始化失败");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "事件循环创建失败");

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "WiFi 初始化失败");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_event_handler, NULL),
                        TAG, "注册 WiFi 事件失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &wifi_event_handler, NULL),
                        TAG, "注册 IP 事件失败");

    /* 热点名带上 MAC 后两位，周围多个设备时不会搞混 */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    wifi_config_t ap_cfg = { 0 };
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len      = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel       = 1;
    ap_cfg.ap.max_connection = 4;
    /* 开放热点：连上才能配网，所以密码这一步在网页里做 */
    ap_cfg.ap.authmode      = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "设置 APSTA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "设置 AP 参数失败");

    /* 定时器必须在使用前创建：STA_START 事件可能在 esp_wifi_start()
       之后立即投递，此时 wifi_prov_connect() 会用到这些定时器。 */
    const esp_timer_create_args_t targs = {
        .callback = on_connect_timeout,
        .name     = "conn_timeout",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_timeout), TAG, "创建超时定时器失败");

    const esp_timer_create_args_t rargs = {
        .callback = on_retry_timer,
        .name     = "conn_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&rargs, &s_retry_timer), TAG, "创建重连定时器失败");

    /* 该赋值必须位于 esp_wifi_start() 之前。
     *
     * STA_START 事件可能早于 esp_wifi_start() 返回（约 773ms 对 877ms）。
     * 事件中的 wifi_prov_connect() 已将状态置为 CONNECTING，
     * 若此时再赋值，会把状态覆盖回 AP；一旦连接失败，断开事件发现状态为 AP
     * 便直接返回，设备将停留在"等待配网"，只能重新配网。
     * 这曾导致"每次上电都要重新配 Wi-Fi"，与 NVS 配置丢失无关。 */
    s_state = WIFI_STATE_AP;

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "启动 WiFi 失败");

    /* WiFi 省电模式：MIN_MODEM（轻度休眠）。
     *
     * 三档区别：
     *   WIFI_PS_NONE       不省电，吞吐最高（传输 4MB 音频约快 30%），但功耗大
     *   WIFI_PS_MIN_MODEM  轻度休眠（选用此项，功耗与速度的平衡点）
     *   WIFI_PS_MAX_MODEM  深度休眠，最省电，但延迟大、连接易掉线
     *
     * 本设备为电池供电的播放器，续航优先于传输速度，因此选择中间档。
     * 若传输歌曲过慢，可临时改为 WIFI_PS_NONE 进行对比。 */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi 省电模式: MIN_MODEM（轻度休眠）");

    /* 自动连接不在此处执行，而交给 WIFI_EVENT_STA_START 事件处理；
       该事件触发时 STA 才真正就绪（在 esp_wifi_start() 之后立即连接会失败）。 */
    ESP_LOGI(TAG, "热点 [%s] 已开启，手机连上后打开 http://192.168.4.1", s_ap_ssid);

    return ESP_OK;
}

/* ================================================================= */
/* 在线模式：真正断开 / 重连路由器                                    */
/* ================================================================= */
void wifi_prov_set_online(bool online)
{
    if (s_online == online) {
        return;
    }
    s_online = online;

    if (!online) {
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
        }
        esp_wifi_disconnect();
        s_ip[0]  = '\0';
        s_state  = WIFI_STATE_AP;      /* 当作未连接；断开事件看到 AP 会忽略 */
        ESP_LOGI(TAG, "在线模式关闭：断开路由器（热点保留，仍可配网）");
    } else {
        /* 不能直接 esp_wifi_connect()：那样 STA 配置可能还是空的，
           日志里 SSID 会是 []。走正常的连接流程更稳。 */
        char ssid[33] = { 0 };
        char pwd[65]  = { 0 };
        if (wifi_prov_load(ssid, sizeof(ssid), pwd, sizeof(pwd))) {
            ESP_LOGI(TAG, "在线模式打开：重新连接 [%s]", ssid);
            wifi_prov_connect(ssid, pwd);
        } else {
            s_state = WIFI_STATE_AP;
            ESP_LOGW(TAG, "在线模式打开，但没有保存过网络");
        }
    }
}

bool wifi_prov_online(void)
{
    return s_online;
}

void wifi_prov_set_online_flag(bool online)
{
    s_online = online;
}
