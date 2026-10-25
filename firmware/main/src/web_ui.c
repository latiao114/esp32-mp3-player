/*
 * web_ui —— 配网 + 音乐库 HTTP 服务器
 *
 * 按【访问来源】分流页面，这一点很重要：
 *
 *   客户端来源            访问 /  得到          说明
 *   ─────────────────────────────────────────────────────────────
 *   192.168.4.x（连热点）  配网页面            只有连上设备热点才能改配置
 *   其他（家里局域网）      音乐库 / 下载页      日常使用走这里
 *
 * 为什么这么做：热点是开放网络，但【家里局域网里的其他人】不应该能改你的
 * 设备配置。所以 /scan、/connect 这类写操作只在热点网段可用。
 */
#include "web_ui.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"

#include "music_api.h"
#include "sd_card.h"
#include "wifi_prov.h"

static const char *TAG = "web_ui";

/* 由 CMake 的 EMBED_FILES 生成 */
extern const char index_html_start[]    asm("_binary_index_html_start");
extern const char index_html_end[]      asm("_binary_index_html_end");
extern const char download_html_start[] asm("_binary_download_html_start");
extern const char download_html_end[]   asm("_binary_download_html_end");

static httpd_handle_t s_server;

#define MAX_SCAN_APS   24
#define JSON_BUF_SIZE  6144
static wifi_ap_record_t s_aps[MAX_SCAN_APS];
static char             s_json[JSON_BUF_SIZE];

/* 设备热点固定是 192.168.4.0/24 */
#define AP_NET  0xC0A80400u
#define AP_MASK 0xFFFFFF00u

/* ================================================================= */
/* 工具                                                              */
/* ================================================================= */
static void json_escape(char *out, size_t out_len, const char *in)
{
    size_t o = 0;
    if (out_len == 0) {
        return;
    }
    for (const char *p = in; *p && o + 8 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, out_len - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void url_decode(char *dst, size_t dlen, const char *src, size_t slen)
{
    size_t d = 0;
    for (size_t i = 0; i < slen && d + 1 < dlen; i++) {
        if (src[i] == '%' && i + 2 < slen) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[d++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[d++] = ' ';
        } else {
            dst[d++] = src[i];
        }
    }
    dst[d] = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t vlen = end ? (size_t)(end - v) : strlen(v);
            url_decode(out, out_len, v, vlen);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* ================================================================= */
/* 来源判断                                                          */
/* ================================================================= */
/*
 * 判断「这个请求是不是连了设备热点的人发来的」。
 *
 * 第一版用 getpeername() 取客户端 IP，但在 ESP-IDF 的 httpd 里拿不到正确值，
 * 结果从热点访问也被判成局域网 —— 配网页打不开，这是 bug。
 *
 * 现在以 HTTP 的 Host 头为主：谁访问 http://192.168.4.1，Host 就是 192.168.4.1，
 * 这是最直接也最可靠的信息。客户端 IP 只作为辅助。
 */
static bool host_is_ap(const char *host)
{
    /* Host 可能是 "192.168.4.1" 或 "192.168.4.1:80" */
    return strncmp(host, "192.168.4.", 10) == 0;
}

static bool is_from_ap_client(httpd_req_t *req)
{
    char host[80];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        if (host_is_ap(host)) {
            return true;
        }
    }

    /* 辅助判断：客户端 IP 落在 192.168.4.0/24 */
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        struct sockaddr_storage ss;
        socklen_t len = sizeof(ss);
        if (getpeername(fd, (struct sockaddr *)&ss, &len) == 0 && ss.ss_family == AF_INET) {
            const struct sockaddr_in *v4 = (const struct sockaddr_in *)&ss;
            uint32_t ip = ntohl(v4->sin_addr.s_addr);
            if ((ip & AP_MASK) == AP_NET) {
                return true;
            }
        }
    }
    return false;
}

/** 配网类写操作只允许热点网段，否则拒绝并说明原因 */
static bool require_ap_access(httpd_req_t *req, const char *what)
{
    if (is_from_ap_client(req)) {
        return true;
    }
    ESP_LOGW(TAG, "拒绝 %s：不是从设备热点访问的", what);
    char msg[192];
    snprintf(msg, sizeof(msg),
             "{\"ok\":false,\"message\":\"改配置只能在连上设备热点 %s 时进行\"}",
             wifi_prov_ap_ssid());
    send_json(req, msg);
    return false;
}

/* ================================================================= */
/* GET /debug  —— 诊断用：把判断依据原样返回，不用连串口也能查         */
/* ================================================================= */
static esp_err_t debug_get(httpd_req_t *req)
{
    char host[80] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));

    char cip[32] = "?";
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        struct sockaddr_storage ss;
        socklen_t len = sizeof(ss);
        if (getpeername(fd, (struct sockaddr *)&ss, &len) == 0 && ss.ss_family == AF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&ss)->sin_addr, cip, sizeof(cip) - 1);
        }
    }

    bool via_ap = is_from_ap_client(req);
    char host_esc[96];
    json_escape(host_esc, sizeof(host_esc), host);

    snprintf(s_json, JSON_BUF_SIZE,
             "{\"host\":\"%s\",\"client_ip\":\"%s\",\"via_ap\":%s,"
             "\"page\":\"%s\",\"ap_ssid\":\"%s\",\"state\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\"}",
             host_esc, cip, via_ap ? "true" : "false",
             via_ap ? "provision" : "library",
             wifi_prov_ap_ssid(), wifi_prov_state_name(),
             wifi_prov_ip(), wifi_prov_connected_ssid());
    return send_json(req, s_json);
}

/* ================================================================= */
/* GET /  —— 按来源给不同页面                                         */
/* ================================================================= */
static esp_err_t root_get(httpd_req_t *req)
{
    const char *page;
    size_t      len;
    bool        via_ap = is_from_ap_client(req);

    if (via_ap) {
        page = index_html_start;
        len  = (size_t)(index_html_end - index_html_start);
    } else {
        page = download_html_start;
        len  = (size_t)(download_html_end - download_html_start);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    ESP_LOGI(TAG, "页面请求 -> %s", via_ap ? "配网页(热点)" : "音乐库(局域网)");
    return httpd_resp_send(req, page, (ssize_t)len);
}

/* ================================================================= */
/* GET /scan  —— 只在热点网段可用                                     */
/* ================================================================= */
static esp_err_t scan_get(httpd_req_t *req)
{
    if (!require_ap_access(req, "扫描 Wi-Fi")) {
        return ESP_OK;
    }

    int n = wifi_prov_scan(s_aps, MAX_SCAN_APS);
    if (n < 0) {
        n = 0;
    }

    size_t o = 0;
    o += (size_t)snprintf(s_json + o, JSON_BUF_SIZE - o, "{\"networks\":[");
    for (int i = 0; i < n && o + 200 < JSON_BUF_SIZE; i++) {
        char ssid[80];
        json_escape(ssid, sizeof(ssid), (const char *)s_aps[i].ssid);
        o += (size_t)snprintf(s_json + o, JSON_BUF_SIZE - o,
                              "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                              (i == 0 ? "" : ","), ssid,
                              (int)s_aps[i].rssi,
                              (s_aps[i].authmode == WIFI_AUTH_OPEN ? 0 : 1));
    }
    snprintf(s_json + o, JSON_BUF_SIZE - o, "]}");

    ESP_LOGI(TAG, "扫描完成，%d 个网络", n);
    return send_json(req, s_json);
}

/* ================================================================= */
/* POST /connect  —— 只在热点网段可用                                 */
/* ================================================================= */
static esp_err_t connect_post(httpd_req_t *req)
{
    if (!require_ap_access(req, "配置 Wi-Fi")) {
        return ESP_OK;
    }

    char body[512] = { 0 };
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - (size_t)total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        total += r;
    }
    body[total] = '\0';

    char ssid[33] = { 0 };
    char pwd[65]  = { 0 };
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return send_json(req, "{\"ok\":false,\"message\":\"没有收到网络名称\"}");
    }
    form_get(body, "password", pwd, sizeof(pwd));

    ESP_LOGI(TAG, "配网请求: ssid=[%s] 密码长度=%d", ssid, (int)strlen(pwd));

    if (wifi_prov_save(ssid, pwd) != ESP_OK) {
        ESP_LOGW(TAG, "保存 NVS 失败（不影响本次连接）");
    }
    if (wifi_prov_connect(ssid, pwd) != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"message\":\"发起连接失败\"}");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"正在连接，请稍候…\"}");
}

/* ================================================================= */
/* GET /status  —— 两种页面都要用，所以不限制来源                      */
/* ================================================================= */
static esp_err_t status_get(httpd_req_t *req)
{
    char ssid[96];
    char reason[128];
    char ap[64];
    json_escape(ssid,   sizeof(ssid),   wifi_prov_connected_ssid());
    json_escape(reason, sizeof(reason), wifi_prov_reason());
    json_escape(ap,     sizeof(ap),     wifi_prov_ap_ssid());

    /* SD 卡真实状态：没插卡或挂载失败时 present=false，前端会显示"未检测到" */
    uint64_t total_bytes = 0;
    uint64_t free_bytes  = 0;
    bool sd_ok = sd_card_ready() &&
                 sd_card_info(&total_bytes, &free_bytes) == ESP_OK;

    /* 排查传输慢用的诊断字段：
       ps   = 省电模式（0=NONE 已关，1=MIN_MODEM，2=MAX_MODEM）
       rssi = 信号强度（dBm，越接近 0 越好） */
    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
    esp_wifi_get_ps(&ps);
    wifi_ap_record_t apinfo;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK) {
        rssi = apinfo.rssi;
    }

    snprintf(s_json, JSON_BUF_SIZE,
             "{\"state\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"reason\":\"%s\",\"ap\":\"%s\","
             "\"ps\":%d,\"rssi\":%d,"
             "\"sd\":{\"present\":%s,\"total\":%llu,\"free\":%llu}}",
             wifi_prov_state_name(), wifi_prov_ip(), ssid, reason, ap,
             (int)ps, rssi,
             sd_ok ? "true" : "false",
             (unsigned long long)(sd_ok ? total_bytes : 0),
             (unsigned long long)(sd_ok ? free_bytes : 0));
    return send_json(req, s_json);
}

/* ================================================================= */
/* GET /files  —— SD 卡里的 MP3 列表                                   */
/* ================================================================= */
static esp_err_t files_get(httpd_req_t *req)
{
    /* 40 个文件 × 约 76 字节 ≈ 3KB。够用，也不至于把内部 RAM 吃掉。
       以后歌多了改成按页返回。 */
    static sd_file_t files[40];

    int n = sd_card_list_mp3(files, 40);

    size_t o = 0;
    o += (size_t)snprintf(s_json + o, JSON_BUF_SIZE - o,
                          "{\"ok\":true,\"count\":%d,\"files\":[", n);
    for (int i = 0; i < n && o + 160 < JSON_BUF_SIZE; i++) {
        char esc[80];
        json_escape(esc, sizeof(esc), files[i].name);
        o += (size_t)snprintf(s_json + o, JSON_BUF_SIZE - o,
                              "%s{\"name\":\"%s\",\"size\":%lu,\"duration\":%lu}",
                              (i == 0 ? "" : ","), esc,
                              (unsigned long)files[i].size,
                              (unsigned long)files[i].duration);
    }
    snprintf(s_json + o, JSON_BUF_SIZE - o, "]}");
    return send_json(req, s_json);
}

/* ================================================================= */
/* GET /play?file=xxx.mp3  —— 把 SD 卡里的 MP3 流式发给浏览器          */
/* ================================================================= */
/*
 * 为什么不是一次性读完再发：
 *   一首 MP3 有几 MB，内部 RAM 装不下，而且浏览器是边下边播的。
 *   这里分块（4KB）读、分块发，内存占用恒定。
 *
 * 为什么要处理 Range：
 *   手机浏览器播放音频时会发 "Range: bytes=0-"，拖动进度条时还会发
 *   "Range: bytes=1048576-"。不支持的话，有的浏览器直接不给播，
 *   有的不能拖进度条。这里按 RFC 返回 206 Partial Content。
 */
static esp_err_t play_get(httpd_req_t *req)
{
    /* 关掉 Nagle：音频数据不希望被攒着等合并，少一次往返是一次 */
    int sock = httpd_req_to_sockfd(req);
    if (sock >= 0) {
        int one = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    char query[256];
    char enc[192];
    char name[128] = { 0 };

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", enc, sizeof(enc)) != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"message\":\"缺少文件名\"}");
    }
    url_decode(name, sizeof(name), enc, strlen(enc));

    /* 安全：只允许根目录下的单个文件名，挡住 ../ 之类 */
    if (name[0] == '\0' || strstr(name, "..") ||
        strchr(name, '/') || strchr(name, '\\')) {
        return send_json(req, "{\"ok\":false,\"message\":\"非法文件名\"}");
    }

    char path[192];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return send_json(req, "{\"ok\":false,\"message\":\"文件不存在\"}");
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return send_json(req, "{\"ok\":false,\"message\":\"文件是空的\"}");
    }

    /* ── 解析 Range ── */
    long start = 0;
    long end   = size - 1;
    char range[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
        long s1 = 0;
        long e1 = -1;
        int  n  = sscanf(range, "bytes=%ld-%ld", &s1, &e1);
        if (n >= 1 && s1 >= 0 && s1 < size) {
            start = s1;
            if (n == 2 && e1 >= s1 && e1 < size) {
                end = e1;
            }
        }
    }

    httpd_resp_set_type(req, "audio/mpeg");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (start > 0 || end < size - 1) {
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld", start, end, size);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", cr);
    }

    fseek(f, start, SEEK_SET);

    long len = end - start + 1;

    /* 响应必须带 Content-Length。
     *
     * httpd_resp_send_chunk() 使用 chunked 编码，浏览器拿不到总长度，
     * 无法建立可 seek 的播放时间轴，表现为拖动进度条后跳回开头
     * （响应头中的 content-length 为 undefined）。
     *
     * 因此这里把该段数据整体读入 PSRAM，再用 httpd_resp_send() 一次发送，
     * httpd 会自动附加 Content-Length。
     * 4MB 的歌曲可以放入 8MB PSRAM；内存不足时才退回 chunked（此时不支持 seek）。
     */
    char *buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc((size_t)len);
    }

    if (!buf) {
        ESP_LOGW(TAG, "缓冲 %ld 字节失败，退回分块发送（seek 可能不可用）", len);
        buf = malloc(4096);
        if (!buf) {
            fclose(f);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        }
        long remain = len;
        while (remain > 0) {
            size_t want = (remain > 4096) ? 4096 : (size_t)remain;
            size_t got  = fread(buf, 1, want, f);
            if (got == 0) {
                break;
            }
            if (httpd_resp_send_chunk(req, buf, got) != ESP_OK) {
                free(buf);
                fclose(f);
                return ESP_FAIL;
            }
            remain -= (long)got;
        }
        free(buf);
        fclose(f);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    int64_t t_read0 = esp_timer_get_time();
    size_t got = fread(buf, 1, (size_t)len, f);
    int64_t t_read1 = esp_timer_get_time();
    fclose(f);

    int64_t t_send0 = esp_timer_get_time();
    esp_err_t err = httpd_resp_send(req, buf, (ssize_t)got);
    int64_t t_send1 = esp_timer_get_time();
    free(buf);

    /* 这两行是排查"切歌慢"用的：读得快还是发得慢，一看就知道 */
    ESP_LOGI(TAG, "音频 %ld 字节：读 %lld ms，发 %lld ms",
             len, (t_read1 - t_read0) / 1000, (t_send1 - t_send0) / 1000);
    return err;
}

/* ================================================================= */
/* GET /api/search?q=关键词  —— 在线搜歌（设备代理请求 iTunes）        */
/* ================================================================= */
static esp_err_t api_search_get(httpd_req_t *req)
{
    char query[320];
    char kw_raw[128] = { 0 };
    char kw[128]     = { 0 };
    char off[16]     = { 0 };
    int  offset      = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        /* httpd_query_key_value 返回的是【未解码】的原始参数串。
           浏览器发送中文时为 %E5%91%A8%E6%9D%B0...；若此处不解码，
           music_search() 会把它当普通文本再次 url_encode，
           变成 %25E5%2591...（双重编码），iTunes 收到的是乱码，
           搜索「周杰伦」会返回无关结果。
           因此必须先用 url_decode 还原为 UTF-8 再向下传递。 */
        if (httpd_query_key_value(query, "q", kw_raw, sizeof(kw_raw)) == ESP_OK) {
            url_decode(kw, sizeof(kw), kw_raw, strlen(kw_raw));
        }
        if (httpd_query_key_value(query, "offset", off, sizeof(off)) == ESP_OK) {
            offset = atoi(off);
            if (offset < 0)    offset = 0;
            /* 最多翻 5 页（每页 20 条）。后端每次要多拉 offset+20 条，
               再大就既慢又费内存；offset=100 时后端会返回空数组，
               前端的"下一页"随即变灰，正好收尾。 */
            if (offset > 100)  offset = 100;
        }
    }
    if (kw[0] == '\0') {
        return send_json(req, "{\"ok\":false,\"message\":\"没有收到搜索词\"}");
    }

    char *json = music_search(kw, 20, offset);
    if (!json) {
        return send_json(req, "{\"ok\":false,\"message\":\"搜索失败\"}");
    }
    esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

/* ================================================================= */
/* GET /api/suggest  —— 首页推荐                                      */
/* ================================================================= */
static esp_err_t api_suggest_get(httpd_req_t *req)
{
    char *json = music_suggest(20);
    if (!json) {
        return send_json(req, "{\"ok\":false,\"message\":\"获取推荐失败\"}");
    }
    esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

/* ================================================================= */
/* favicon                                                            */
/* ================================================================= */
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ================================================================= */
/* 启动                                                              */
/* ================================================================= */
esp_err_t web_ui_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.max_uri_handlers  = 16;
    cfg.stack_size        = 8192;
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "HTTP 服务器启动失败");

    const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = root_get,            .user_ctx = NULL },
        { .uri = "/scan",            .method = HTTP_GET,  .handler = scan_get,            .user_ctx = NULL },
        { .uri = "/connect",         .method = HTTP_POST, .handler = connect_post,        .user_ctx = NULL },
        { .uri = "/status",          .method = HTTP_GET,  .handler = status_get,          .user_ctx = NULL },
        { .uri = "/files",           .method = HTTP_GET,  .handler = files_get,           .user_ctx = NULL },
        { .uri = "/play",            .method = HTTP_GET,  .handler = play_get,            .user_ctx = NULL },
        { .uri = "/debug",           .method = HTTP_GET,  .handler = debug_get,           .user_ctx = NULL },
        { .uri = "/api/search",      .method = HTTP_GET,  .handler = api_search_get,      .user_ctx = NULL },
        { .uri = "/api/suggest",     .method = HTTP_GET,  .handler = api_suggest_get,     .user_ctx = NULL },
        { .uri = "/favicon.ico",     .method = HTTP_GET,  .handler = favicon_get,         .user_ctx = NULL },
        { .uri = "/generate_204",        .method = HTTP_GET, .handler = root_get, .user_ctx = NULL },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL },
        { .uri = "/ncsi.txt",            .method = HTTP_GET, .handler = root_get, .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &uris[i]),
                            TAG, "注册路由 %s 失败", uris[i].uri);
    }

    ESP_LOGI(TAG, "HTTP 服务器已启动");
    ESP_LOGI(TAG, "  连热点      -> http://192.168.4.1      （配网页面）");
    ESP_LOGI(TAG, "  同一局域网  -> http://<设备IP>          （音乐库页面）");
    return ESP_OK;
}

void web_ui_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
