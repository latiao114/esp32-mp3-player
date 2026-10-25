/*
 * music_api —— 在线音乐搜索实现
 */

#include "music_api.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "music_api";

/* 单次请求最多拉取 100 条（offset + 20 条），响应体较大，
   因此超时与缓冲上限相应放宽。 */
#define HTTP_TIMEOUT_MS   25000
#define MAX_BODY_BYTES    (192 * 1024)

/* iTunes 单次最多返回 200 条，但 200 条约 280KB JSON；
   这里只取 100 条（约 140KB），足够翻 5 页。 */
#define ITUNES_MAX_LIMIT  100

/* ================================================================= */
/* 工具                                                              */
/* ================================================================= */
static void url_encode(const char *src, char *dst, size_t dst_len)
{
    size_t d = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && d + 4 < dst_len; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            dst[d++] = (char)*p;
        } else {
            d += (size_t)snprintf(dst + d, dst_len - d, "%%%02X", *p);
        }
    }
    dst[d] = '\0';
}

/* ================================================================= */
/* cJSON 的内存来源                                                   */
/* ================================================================= */
/* cJSON 默认走 malloc，全部落在内部 RAM。一份 100 条的搜索结果解析完
   大约要 400KB，内部 RAM 根本扛不住（翻到第 3 页就会失败）。
   这里把 cJSON 的分配器指到 PSRAM，内部 RAM 只留给协议栈和任务栈。
   没开 PSRAM 时 heap_caps_malloc 返回 NULL，自动退回 malloc，不会崩。 */
static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void psram_free(void *p)
{
    heap_caps_free(p);   /* 内部 RAM 和 PSRAM 的指针都能正确释放 */
}

static void ensure_cjson_hooks(void)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = psram_free };
    cJSON_InitHooks(&hooks);
}

/* 安全取字符串字段：字段不存在或类型不对都返回空串，不返回 NULL */
static const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

static double jnum(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : 0.0;
}

static char *err_json(const char *msg)
{
    ensure_cjson_hooks();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "message", msg ? msg : "未知错误");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

/* ================================================================= */
/* HTTP GET                                                          */
/* ================================================================= */
/* 优先使用 PSRAM，不可用时退回内部 RAM。
   通过 *in_psram 记录内存来源：realloc 不能跨堆进行，
   一旦记错就会越界访问。 */
static void *buf_alloc(size_t n, bool *in_psram)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        *in_psram = true;
        return p;
    }
    *in_psram = false;
    return malloc(n);
}

static void *buf_realloc(void *p, size_t n, bool in_psram)
{
    if (in_psram) {
        return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return realloc(p, n);
}

static char *http_get(const char *url, int *out_len)
{
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 用 IDF 内置根证书验 HTTPS */
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .buffer_size       = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client 初始化失败");
        return NULL;
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32-MP3/1.0");

    char *buf     = NULL;
    int   total   = 0;
    int   cap     = 0;
    bool  in_psram = false;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "连接服务器失败");
        goto done;
    }

    /* 服务器给了 Content-Length 就按它一次分配到位；
       没给（chunked）就先给 8KB，后面边读边翻倍。
       这样小响应不会白占内存，大响应也不用一开始就赌 192KB。 */
    int declared = esp_http_client_fetch_headers(client);
    int initial  = (declared > 0 && declared <= MAX_BODY_BYTES) ? declared : 8192;

    if (initial < 4096) {
        initial = 4096;
    }
    buf = buf_alloc((size_t)initial + 1, &in_psram);
    if (!buf) {
        ESP_LOGE(TAG, "响应缓冲分配失败 (%d 字节)", initial);
        goto done;
    }
    cap = initial;

    while (total < MAX_BODY_BYTES) {
        int room = cap - total - 1;
        if (room <= 0) {
            int next = cap * 2;
            if (next > MAX_BODY_BYTES) {
                next = MAX_BODY_BYTES;
            }
            if (next <= cap) {
                break;
            }
            char *bigger = buf_realloc(buf, (size_t)next + 1, in_psram);
            if (!bigger) {
                ESP_LOGW(TAG, "缓冲扩容失败，已收到 %d 字节", total);
                break;
            }
            buf = bigger;
            cap = next;
            room = cap - total - 1;
        }
        int r = esp_http_client_read(client, buf + total, room);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0';
    *out_len = total;

    if (total >= MAX_BODY_BYTES) {
        ESP_LOGW(TAG, "响应到达上限 %d 字节，可能被截断", MAX_BODY_BYTES);
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return buf;
}

/* ================================================================= */
/* 解析 iTunes 返回                                                   */
/* ================================================================= */
static char *parse_itunes(const char *json, int limit, int offset)
{
    ensure_cjson_hooks();
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return NULL;
    }

    cJSON *results = cJSON_GetObjectItem(root, "results");
    cJSON *out = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(out, "songs");
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "offset", offset);

    int total = cJSON_GetArraySize(results);
    int kept  = 0;
    int valid = 0;

    /* 翻页逻辑的关键：顺序不可颠倒。
     *
     *   iTunes 不支持 offset，只能多取一些再在本地切页。而"第几条"
     *   必须按【可播放的条目】计数 —— 先过滤掉没有 previewUrl 的条目，
     *   再数到第 offset 个开始收集。若反过来（先按原始下标切页再过滤），
     *   只要某一页混入一个无试听条目，后续所有页都会整体错位，
     *   出现漏歌或重复。 */
    for (int i = 0; i < total && kept < limit; i++) {
        cJSON *it = cJSON_GetArrayItem(results, i);
        const char *preview = jstr(it, "previewUrl");
        if (preview[0] == '\0') {
            continue;   /* 没有试听地址的结果没用，不算一条 */
        }

        /* 落在前面几页的，跳过不返回 */
        if (valid++ < offset) {
            continue;
        }

        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name",    jstr(it, "trackName"));
        cJSON_AddStringToObject(s, "artist",  jstr(it, "artistName"));
        cJSON_AddStringToObject(s, "album",   jstr(it, "collectionName"));
        cJSON_AddStringToObject(s, "preview", preview);
        cJSON_AddStringToObject(s, "cover",   jstr(it, "artworkUrl100"));
        cJSON_AddNumberToObject(s, "duration",
                                (int)(jnum(it, "trackTimeMillis") / 1000.0));
        cJSON_AddItemToArray(arr, s);
        kept++;
    }

    /* 返回本页实际条数，前端据此判断还有没有下一页 */
    cJSON_AddNumberToObject(out, "count", kept);

    char *ret = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "解析完成：offset=%d 原始 %d 条，可用 %d 条，本页 %d 条",
             offset, total, valid, kept);
    return ret;
}

/* ================================================================= */
/* 对外接口                                                          */
/* ================================================================= */
/* ── 翻页的实现要点 ───────────────────────────────────────────────
 * iTunes 对同一个关键词，limit 不同时返回的结果集也不同，并非简单截断。
 * 例如搜索「周杰伦」：limit=20 只返回 16 条，limit=40 返回 40 条，
 * 且两批结果的第 2 首分别是「屋頂」和「告白气球」，顺序完全不同。
 *
 * 因此"第 N 页就拉 offset+20 条"会导致串页：第 1 页显示 16 首，
 * 第 2 页却是另一批歌。可靠做法是：每次都用同一 limit 拉取同一批结果，
 * 再在本地切页。
 *
 * 每次联网较慢，所以把原始响应缓存在 PSRAM：
 * 同一关键词只有第一页需要联网，后续页直接从缓存切出。
 */
static char *s_cache_body;
static char  s_cache_kw[128];

char *music_search(const char *keyword, int limit, int offset)
{
    if (!keyword || keyword[0] == '\0') {
        return err_json("搜索词不能为空");
    }
    if (limit <= 0 || limit > 50) {
        limit = 20;
    }
    if (offset < 0) {
        offset = 0;
    }

    char *body;

    if (s_cache_body && strcmp(s_cache_kw, keyword) == 0) {
        ESP_LOGI(TAG, "命中缓存: %s (offset=%d)", keyword, offset);
        body = s_cache_body;
    } else {
        char enc[512];
        url_encode(keyword, enc, sizeof(enc));

        /* 固定拉 ITUNES_MAX_LIMIT 条，保证每一页看到的是同一份结果集 */
        char url[672];
        snprintf(url, sizeof(url),
                 "https://itunes.apple.com/search?term=%s&media=music&entity=song"
                 "&limit=%d&country=us",
                 enc, ITUNES_MAX_LIMIT);

        ESP_LOGI(TAG, "搜索: %s (offset=%d, 固定拉 %d 条)", keyword, offset, ITUNES_MAX_LIMIT);

        int body_len = 0;
        body = http_get(url, &body_len);
        if (!body) {
            return err_json("连接音乐服务器失败，检查设备是否联网");
        }
        ESP_LOGI(TAG, "响应 %d 字节", body_len);

        /* 换关键词了，旧的缓存没用了，释放掉换成新的 */
        if (s_cache_body) {
            heap_caps_free(s_cache_body);
        }
        s_cache_body = body;
        strncpy(s_cache_kw, keyword, sizeof(s_cache_kw) - 1);
        s_cache_kw[sizeof(s_cache_kw) - 1] = '\0';
    }

    char *ret = parse_itunes(body, limit, offset);
    if (!ret) {
        return err_json("音乐服务器返回了无法解析的内容");
    }
    return ret;
}

char *music_suggest(int limit)
{
    /* 轮换几个种子词，首页每次刷新都有不同内容 */
    static const char *seeds[] = {
        "top hits", "chinese pop", "piano", "acoustic", "jazz", "electronic"
    };
    static unsigned idx = 0;
    const char *seed = seeds[idx++ % (sizeof(seeds) / sizeof(seeds[0]))];
    return music_search(seed, limit, 0);
}
